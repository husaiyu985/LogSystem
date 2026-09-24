#include "client/cloud_client.h"

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("CloudLog"));
    QCoreApplication::setApplicationName(QStringLiteral("CloudLog"));
    QQmlApplicationEngine engine;
    CloudClient client;
    engine.rootContext()->setContextProperty(QStringLiteral("cloudClient"), &client);
    engine.load(QUrl(QStringLiteral("qrc:/qml/main.qml")));
    if (engine.rootObjects().isEmpty()) return 1;
    return app.exec();
}
