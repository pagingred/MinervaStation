#include <QApplication>
#include <QIcon>
#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("MinervaStation");
    app.setOrganizationName("RedLine Solutions, LLC.");
    app.setWindowIcon(QIcon(":/icon.png"));

    MainWindow w;
    w.show();
    return app.exec();
}
