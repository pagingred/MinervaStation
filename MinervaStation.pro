QT += core gui widgets network websockets
CONFIG += c++20
TARGET = MinervaStation
TEMPLATE = app

SOURCES += $$files($$PWD/*.cpp)
HEADERS += $$files($$PWD/*.h)
FORMS += mainwindow.ui

win32: RC_FILE = app.rc
RESOURCES += resources.qrc
