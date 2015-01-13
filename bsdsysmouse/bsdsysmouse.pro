TARGET = qbsdsysmouseplugin

PLUGIN_TYPE = generic
PLUGIN_EXTENDS = -
PLUGIN_CLASS_NAME = QBsdSysMousePlugin
load(qt_plugin)

QT += core-private gui-private

HEADERS = qbsdsysmouse.h
SOURCES = main.cpp \
         qbsdsysmouse.cpp

OTHER_FILES += \
    qbsdsysmouse.json

