QT += core gui widgets network
CONFIG += c++20
TARGET = MinervaStation
TEMPLATE = app

SOURCES += $$files($$PWD/*.cpp)
HEADERS += $$files($$PWD/*.h) \
    jobinfo.h \
    minervaconfig.h
FORMS += mainwindow.ui

win32: RC_FILE = app.rc
macx:  ICON = icon.icns
RESOURCES += resources.qrc
