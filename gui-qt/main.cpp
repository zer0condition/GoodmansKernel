#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QStyleFactory>
#include "mainwindow.h"

int main(int argc, char** argv)
{
    QApplication::setStyle("Fusion");
    QApplication app(argc, argv);
    app.setOrganizationName("goodmans");
    app.setApplicationName("The Goodmans Kernel");

    QFontDatabase::addApplicationFont(":/fonts/segoeui");

    QFile qss(":/style.qss");
    if (qss.open(QIODevice::ReadOnly | QIODevice::Text)) {
        app.setStyleSheet(QString::fromUtf8(qss.readAll()));
        qss.close();
    }

    MainWindow w;
    w.show();
    return app.exec();
}
