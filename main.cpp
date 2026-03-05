#include <QApplication>
#include <QIcon>
#include <QThread>
#include "mainwindow.h"
#include "splashscreen.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("MinervaStation");
    app.setOrganizationName("RedLine Solutions, LLC.");
    app.setWindowIcon(QIcon(":/icon.png"));

    SplashScreen splash;
    splash.show();
    app.processEvents();

    splash.SetStatus("Initializing...");
    splash.SetProgress(20);

    splash.SetStatus("Loading settings...");
    splash.SetProgress(50);

    MainWindow w;

    splash.SetStatus("Ready");
    splash.SetProgress(100);
    QThread::msleep(200);

    splash.Finish(&w);

    return app.exec();
}
