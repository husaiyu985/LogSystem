#include "client/cloud_client.h"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextDocument>
#include <QUrlQuery>
#include <QUuid>

namespace {
QString response_error(QNetworkReply* reply) {
    const auto document = QJsonDocument::fromJson(reply->readAll());
    if (document.isObject()) {
        const auto message = document.object().value(QStringLiteral("error")).toString();
        if (!message.isEmpty()) return message;
    }
    return reply->errorString();
}

QString compact_title(QString text) {
    text = text.simplified();
    if (text.isEmpty()) return QStringLiteral("新对话");
    constexpr int maximumLength = 22;
    return text.size() > maximumLength ? text.left(maximumLength) + QStringLiteral("…") : text;
}

QJsonObject empty_conversation(const QString& id) {
    const QString now = QDateTime::currentDateTime().toString(Qt::ISODate);
    return QJsonObject{{QStringLiteral("id"), id},
                       {QStringLiteral("title"), QStringLiteral("新对话")},
                       {QStringLiteral("created_at"), now},
                       {QStringLiteral("updated_at"), now},
                       {QStringLiteral("messages"), QJsonArray{}}};
}
}

CloudClient::CloudClient(QObject* parent) : QObject(parent) {
    auto store = loadConversationStore();
    activeConversationId_ = store.value(QStringLiteral("active_id")).toString();
    const auto conversations = store.value(QStringLiteral("conversations")).toArray();
    if (activeConversationId_.isEmpty() && !conversations.isEmpty()) {
        activeConversationId_ = conversations.first().toObject().value(QStringLiteral("id")).toString();
        store.insert(QStringLiteral("active_id"), activeConversationId_);
    }
    if (!conversations.isEmpty()) saveConversationStore(store);
}

QString CloudClient::apiUrl(const QString& path) const {
    return serverBaseUrl_ + path;
}

void CloudClient::refreshFiles() {
    requestFileList(owner_, {}, false);
}

void CloudClient::refreshArchives() {
    requestFileList(QStringLiteral("system"), QStringLiteral("system-logs"), true);
}

void CloudClient::requestFileList(const QString& owner, const QString& parent, bool archives) {
    QUrl url(apiUrl(QStringLiteral("/api/files")));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("owner"), owner);
    if (!parent.isEmpty()) query.addQueryItem(QStringLiteral("parent"), parent);
    url.setQuery(query);
    auto* reply = network_.get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, archives] {
        const auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit operationError(QStringLiteral("无法读取文件列表：") + reply->errorString());
        } else {
            const auto document = QJsonDocument::fromJson(data);
            if (!document.isArray())
                emit operationError(QStringLiteral("文件列表响应格式错误"));
            else if (archives)
                emit archivesReceived(document.array().toVariantList());
            else
                emit filesReceived(document.array().toVariantList());
        }
        reply->deleteLater();
    });
}

void CloudClient::refreshSystemLog() {
    if (systemLogRequestPending_) return;
    systemLogRequestPending_ = true;
    auto* reply = network_.get(QNetworkRequest(QUrl(apiUrl(QStringLiteral("/api/system-log")))));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        systemLogRequestPending_ = false;
        if (reply->error() != QNetworkReply::NoError) {
            if (!systemLogErrorReported_) {
                systemLogErrorReported_ = true;
                emit operationError(QStringLiteral("无法读取系统实时日志：") + reply->errorString());
            }
        } else {
            systemLogErrorReported_ = false;
            emit systemLogReceived(QString::fromUtf8(reply->readAll()));
        }
        reply->deleteLater();
    });
}

void CloudClient::chooseAndUpload(const QString& storageMode) {
    const QString path = QFileDialog::getOpenFileName(
        nullptr, QStringLiteral("选择要上传的文件"), QDir::homePath(),
        QStringLiteral("日志和文本文件 (*.log *.txt);;所有文件 (*.*)"));
    if (path.isEmpty()) return;

    auto* file = new QFile(path);
    if (!file->open(QIODevice::ReadOnly)) {
        emit operationError(QStringLiteral("无法读取文件：") + path);
        delete file;
        return;
    }
    constexpr qint64 maximumUpload = 128LL * 1024 * 1024;
    if (file->size() > maximumUpload) {
        emit operationError(QStringLiteral("文件超过 128 MiB，当前服务不支持上传"));
        delete file;
        return;
    }
    const QString fileName = QFileInfo(*file).fileName();

    QUrl url(apiUrl(QStringLiteral("/api/upload")));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("owner"), owner_);
    query.addQueryItem(QStringLiteral("name"), fileName);
    query.addQueryItem(QStringLiteral("storage"), storageMode);
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
    auto* reply = network_.post(request, file);
    file->setParent(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, fileName] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 201) {
            emit uploadSucceeded(fileName);
            refreshFiles();
        } else if (status == 409) {
            emit duplicateDetected(fileName);
        } else {
            emit operationError(QStringLiteral("上传失败：") + response_error(reply));
        }
        reply->deleteLater();
    });
}

void CloudClient::downloadFile(const QString& id, const QString& suggestedName,
                               const QString& owner) {
    const QString destination = QFileDialog::getSaveFileName(
        nullptr, QStringLiteral("保存下载文件"), QDir::home().filePath(suggestedName));
    if (destination.isEmpty()) return;

    QUrl url(apiUrl(QStringLiteral("/api/download/") + id));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("owner"), owner.isEmpty() ? owner_ : owner);
    url.setQuery(query);
    auto* output = new QSaveFile(destination);
    if (!output->open(QIODevice::WriteOnly)) {
        emit operationError(QStringLiteral("无法创建下载文件：") + destination);
        delete output;
        return;
    }
    auto* reply = network_.get(QNetworkRequest(url));
    output->setParent(reply);
    connect(reply, &QIODevice::readyRead, this, [reply, output] {
        const auto chunk = reply->readAll();
        if (!chunk.isEmpty() && output->write(chunk) != chunk.size()) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, output, destination] {
        if (reply->error() != QNetworkReply::NoError) {
            output->cancelWriting();
            emit operationError(QStringLiteral("下载失败：") + reply->errorString());
        } else {
            const auto finalChunk = reply->readAll();
            const bool finalWrite = finalChunk.isEmpty() || output->write(finalChunk) == finalChunk.size();
            if (!finalWrite || !output->commit())
                emit operationError(QStringLiteral("无法保存到：") + destination);
            else
                emit downloadSucceeded(destination);
        }
        reply->deleteLater();
    });
}

void CloudClient::previewFile(const QString& id, const QString& suggestedName,
                              const QString& owner) {
    QUrl url(apiUrl(QStringLiteral("/api/preview/") + id));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("owner"), owner.isEmpty() ? owner_ : owner);
    url.setQuery(query);
    auto* reply = network_.get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, suggestedName] {
        if (reply->error() != QNetworkReply::NoError) {
            emit operationError(QStringLiteral("预览失败：") + reply->errorString());
        } else {
            const bool truncated = reply->rawHeader("X-Content-Truncated") == "true";
            emit previewReceived(suggestedName, QString::fromUtf8(reply->readAll()), truncated);
        }
        reply->deleteLater();
    });
}

void CloudClient::deleteFile(const QString& id) {
    QUrl url(apiUrl(QStringLiteral("/api/files/") + id));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("owner"), owner_);
    url.setQuery(query);
    QNetworkRequest request(url);
    auto* reply = network_.deleteResource(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (reply->error() == QNetworkReply::NoError)
            refreshFiles();
        else
            emit operationError(QStringLiteral("删除失败：") + reply->errorString());
        reply->deleteLater();
    });
}

void CloudClient::sendAssistant(const QString& message, const QString& fileId,
                                 const QString& fileOwner) {
    const QString cleaned = message.trimmed();
    if (cleaned.isEmpty()) return;
    if (assistantReply_) {
        emit operationError(QStringLiteral("AI 正在处理上一条请求，请稍候或新建对话。"));
        return;
    }

    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    activeAssistantRequestId_ = requestId;
    emit assistantRequestStarted(requestId, cleaned);
    appendHistory(QStringLiteral("user"), cleaned);

    const QString effectiveOwner = fileOwner.isEmpty() ? owner_ : fileOwner;
    QJsonArray context;
    const auto history = loadHistory();
    qsizetype contextCharacters = 0;
    for (auto iterator = history.crbegin(); iterator != history.crend() && context.size() < 12;
         ++iterator) {
        const auto item = iterator->toMap();
        const QString role = item.value(QStringLiteral("role")).toString();
        const QString text = item.value(QStringLiteral("text")).toString();
        if ((role != QStringLiteral("user") && role != QStringLiteral("assistant")) ||
            text.isEmpty()) continue;
        if (!context.isEmpty() && contextCharacters + text.size() > 60000) break;
        context.insert(0, QJsonObject{{QStringLiteral("role"), role},
                                      {QStringLiteral("content"), text}});
        contextCharacters += text.size();
    }
    QJsonObject body{{QStringLiteral("message"), cleaned},
                     {QStringLiteral("owner"), effectiveOwner},
                     {QStringLiteral("file_id"), fileId},
                     {QStringLiteral("request_id"), requestId},
                     {QStringLiteral("conversation_id"), activeConversationId_},
                     {QStringLiteral("messages"), context}};
    QUrl url(apiUrl(QStringLiteral("/api/assistant")));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("owner"), effectiveOwner);
    url.setQuery(query);
    QNetworkRequest request{url};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setRawHeader("Cache-Control", "no-cache, no-store");
    auto* reply = network_.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    assistantReply_ = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, requestId] {
        if (requestId != activeAssistantRequestId_) {
            reply->deleteLater();
            return;
        }

        assistantReply_.clear();
        const auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit operationError(QStringLiteral("AI 请求失败：") + reply->errorString());
        } else {
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(data, &parseError);
            const auto object = document.object();
            const QString returnedRequestId = object.value(QStringLiteral("request_id")).toString();
            const QString answer = object.value(QStringLiteral("reply")).toString();
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                emit operationError(QStringLiteral("AI 响应 JSON 无效：") + parseError.errorString());
            } else if (returnedRequestId != requestId) {
                emit operationError(QStringLiteral("AI 响应与当前问题不匹配，已丢弃过期响应。"));
            } else if (answer.isEmpty()) {
                emit operationError(QStringLiteral("AI 响应内容为空"));
            } else {
                appendHistory(QStringLiteral("assistant"), answer);
                emit assistantReplyReceived(requestId, answer);
            }
        }
        activeAssistantRequestId_.clear();
        emit assistantRequestFinished(requestId);
        reply->deleteLater();
    });
}

QString CloudClient::historyPath() const {
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(directory);
    return QDir(directory).filePath(QStringLiteral("assistant_conversations.json"));
}

QString CloudClient::legacyHistoryPath() const {
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(directory).filePath(QStringLiteral("assistant_history.json"));
}

QJsonObject CloudClient::loadConversationStore() const {
    QFile input(historyPath());
    if (input.open(QIODevice::ReadOnly)) {
        const auto document = QJsonDocument::fromJson(input.readAll());
        if (document.isObject() && document.object().value(QStringLiteral("conversations")).isArray())
            return document.object();
    }

    QJsonObject store{{QStringLiteral("version"), 1},
                      {QStringLiteral("active_id"), QString{}},
                      {QStringLiteral("conversations"), QJsonArray{}}};

    QFile legacy(legacyHistoryPath());
    if (!legacy.open(QIODevice::ReadOnly)) return store;
    const auto legacyDocument = QJsonDocument::fromJson(legacy.readAll());
    if (!legacyDocument.isArray() || legacyDocument.array().isEmpty()) return store;

    const auto legacyMessages = legacyDocument.array();
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject conversation = empty_conversation(id);
    conversation.insert(QStringLiteral("messages"), legacyMessages);
    for (const auto& value : legacyMessages) {
        const auto item = value.toObject();
        if (item.value(QStringLiteral("role")).toString() == QStringLiteral("user")) {
            conversation.insert(QStringLiteral("title"),
                                compact_title(item.value(QStringLiteral("text")).toString()));
            break;
        }
    }
    QJsonArray conversations;
    conversations.append(conversation);
    store.insert(QStringLiteral("active_id"), id);
    store.insert(QStringLiteral("conversations"), conversations);
    return store;
}

bool CloudClient::saveConversationStore(const QJsonObject& store) const {
    QSaveFile output(historyPath());
    if (!output.open(QIODevice::WriteOnly)) return false;
    if (output.write(QJsonDocument(store).toJson(QJsonDocument::Indented)) < 0) return false;
    return output.commit();
}

QVariantList CloudClient::loadHistory() const {
    const auto conversations = loadConversationStore()
        .value(QStringLiteral("conversations")).toArray();
    for (const auto& value : conversations) {
        const auto conversation = value.toObject();
        if (conversation.value(QStringLiteral("id")).toString() == activeConversationId_)
            return conversation.value(QStringLiteral("messages")).toArray().toVariantList();
    }
    return {};
}

QVariantList CloudClient::loadConversations() const {
    QVariantList result;
    const auto conversations = loadConversationStore()
        .value(QStringLiteral("conversations")).toArray();
    for (const auto& value : conversations) {
        const auto conversation = value.toObject();
        const auto messages = conversation.value(QStringLiteral("messages")).toArray();
        QString preview;
        if (!messages.isEmpty())
            preview = messages.last().toObject().value(QStringLiteral("text")).toString().simplified();
        if (preview.size() > 38) preview = preview.left(38) + QStringLiteral("…");

        QVariantMap item;
        const QString id = conversation.value(QStringLiteral("id")).toString();
        item.insert(QStringLiteral("id"), id);
        item.insert(QStringLiteral("title"), conversation.value(QStringLiteral("title")).toString());
        item.insert(QStringLiteral("preview"), preview);
        item.insert(QStringLiteral("updatedAt"), conversation.value(QStringLiteral("updated_at")).toString());
        item.insert(QStringLiteral("messageCount"), messages.size());
        item.insert(QStringLiteral("active"), id == activeConversationId_);
        result.append(item);
    }
    return result;
}

QString CloudClient::activeConversationId() const {
    return activeConversationId_;
}

void CloudClient::appendHistory(const QString& role, const QString& text) {
    auto store = loadConversationStore();
    auto conversations = store.value(QStringLiteral("conversations")).toArray();
    int index = -1;
    for (int i = 0; i < conversations.size(); ++i) {
        if (conversations.at(i).toObject().value(QStringLiteral("id")).toString() ==
            activeConversationId_) {
            index = i;
            break;
        }
    }

    if (index < 0) {
        activeConversationId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        conversations.insert(0, empty_conversation(activeConversationId_));
        index = 0;
    }

    auto conversation = conversations.at(index).toObject();
    auto messages = conversation.value(QStringLiteral("messages")).toArray();
    messages.append(QJsonObject{{QStringLiteral("role"), role},
                                {QStringLiteral("text"), text},
                                {QStringLiteral("time"),
                                 QDateTime::currentDateTime().toString(Qt::ISODate)}});
    while (messages.size() > 200) messages.removeAt(0);

    if (role == QStringLiteral("user") &&
        conversation.value(QStringLiteral("title")).toString() == QStringLiteral("新对话")) {
        conversation.insert(QStringLiteral("title"), compact_title(text));
    }
    conversation.insert(QStringLiteral("messages"), messages);
    conversation.insert(QStringLiteral("updated_at"),
                        QDateTime::currentDateTime().toString(Qt::ISODate));
    conversations.removeAt(index);
    conversations.insert(0, conversation);
    while (conversations.size() > 40) conversations.removeAt(conversations.size() - 1);

    store.insert(QStringLiteral("active_id"), activeConversationId_);
    store.insert(QStringLiteral("conversations"), conversations);
    saveConversationStore(store);
    emit conversationsChanged(loadConversations());
}

void CloudClient::cancelAssistantRequest() {
    activeAssistantRequestId_.clear();
    if (!assistantReply_) return;
    auto* pendingReply = assistantReply_.data();
    assistantReply_.clear();
    pendingReply->abort();
}

void CloudClient::selectConversation(const QString& conversationId) {
    auto store = loadConversationStore();
    const auto conversations = store.value(QStringLiteral("conversations")).toArray();
    for (const auto& value : conversations) {
        const auto conversation = value.toObject();
        if (conversation.value(QStringLiteral("id")).toString() != conversationId) continue;
        cancelAssistantRequest();
        activeConversationId_ = conversationId;
        store.insert(QStringLiteral("active_id"), activeConversationId_);
        saveConversationStore(store);
        emit conversationLoaded(activeConversationId_,
                                conversation.value(QStringLiteral("messages")).toArray().toVariantList());
        emit conversationsChanged(loadConversations());
        return;
    }
}

void CloudClient::deleteConversation(const QString& conversationId) {
    auto store = loadConversationStore();
    auto conversations = store.value(QStringLiteral("conversations")).toArray();
    int index = -1;
    for (int i = 0; i < conversations.size(); ++i) {
        if (conversations.at(i).toObject().value(QStringLiteral("id")).toString() == conversationId) {
            index = i;
            break;
        }
    }
    if (index < 0) return;

    const bool deletedActive = conversationId == activeConversationId_;
    if (deletedActive) cancelAssistantRequest();
    conversations.removeAt(index);
    if (deletedActive) {
        activeConversationId_ = conversations.isEmpty()
            ? QString{}
            : conversations.first().toObject().value(QStringLiteral("id")).toString();
    }
    store.insert(QStringLiteral("active_id"), activeConversationId_);
    store.insert(QStringLiteral("conversations"), conversations);
    saveConversationStore(store);
    emit conversationsChanged(loadConversations());
    if (deletedActive) emit conversationLoaded(activeConversationId_, loadHistory());
}

void CloudClient::clearHistory() {
    newConversation();
}

void CloudClient::newConversation() {
    cancelAssistantRequest();
    auto store = loadConversationStore();
    auto conversations = store.value(QStringLiteral("conversations")).toArray();

    for (const auto& value : conversations) {
        const auto conversation = value.toObject();
        if (conversation.value(QStringLiteral("id")).toString() == activeConversationId_ &&
            conversation.value(QStringLiteral("messages")).toArray().isEmpty()) {
            emit historyCleared();
            emit conversationsChanged(loadConversations());
            return;
        }
    }

    activeConversationId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    conversations.insert(0, empty_conversation(activeConversationId_));
    while (conversations.size() > 40) conversations.removeAt(conversations.size() - 1);
    store.insert(QStringLiteral("active_id"), activeConversationId_);
    store.insert(QStringLiteral("conversations"), conversations);
    saveConversationStore(store);
    emit historyCleared();
    emit conversationsChanged(loadConversations());
}

void CloudClient::copyText(const QString& text, bool markdown) const {
    QString clipboardText = text;
    if (markdown) {
        QTextDocument document;
        document.setMarkdown(text);
        clipboardText = document.toPlainText();
    }
    if (auto* clipboard = QGuiApplication::clipboard()) clipboard->setText(clipboardText);
}
