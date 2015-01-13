/****************************************************************************
**
** Copyright (C) 2014 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the QtGui module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL21$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia. For licensing terms and
** conditions see http://qt.digia.com/licensing. For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file. Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights. These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#ifndef QTSLIB_H
#define QTSLIB_H

#include <qobject.h>
//#include <Qt>
#include <QDataStream>

QT_BEGIN_NAMESPACE

class QSocketNotifier;

struct termios;

namespace QBsdKeyboardMap {
    const quint32 FileMagic = 0x514d4150; // 'QMAP'

    struct Mapping {
        quint16 keycode;
        quint16 unicode;
        quint32 qtcode;
        quint8 modifiers;
        quint8 flags;
        quint16 special;

    };

    enum Flags {
        IsDead     = 0x01,
        IsLetter   = 0x02,
        IsModifier = 0x04,
        IsSystem   = 0x08
    };

    enum System {
        SystemConsoleFirst    = 0x0100,
        SystemConsoleMask     = 0x007f,
        SystemConsoleLast     = 0x017f,
        SystemConsolePrevious = 0x0180,
        SystemConsoleNext     = 0x0181,
        SystemReboot          = 0x0200,
        SystemZap             = 0x0300
    };

    struct Composing {
        quint16 first;
        quint16 second;
        quint16 result;
    };

    enum Modifiers {
        ModPlain   = 0x00,
        ModShift   = 0x01,
        ModAltGr   = 0x02,
        ModControl = 0x04,
        ModAlt     = 0x08,
        ModShiftL  = 0x10,
        ModShiftR  = 0x20,
        ModCtrlL   = 0x40,
        ModCtrlR   = 0x80
        // ModCapsShift = 0x100, // not supported!
    };
}

inline QDataStream &operator>>(QDataStream &ds, QBsdKeyboardMap::Mapping &m)
{
    return ds >> m.keycode >> m.unicode >> m.qtcode >> m.modifiers >> m.flags >> m.special;
}

inline QDataStream &operator<<(QDataStream &ds, const QBsdKeyboardMap::Mapping &m)
{
    return ds << m.keycode << m.unicode << m.qtcode << m.modifiers << m.flags << m.special;
}

inline QDataStream &operator>>(QDataStream &ds, QBsdKeyboardMap::Composing &c)
{
    return ds >> c.first >> c.second >> c.result;
}

inline QDataStream &operator<<(QDataStream &ds, const QBsdKeyboardMap::Composing &c)
{
    return ds << c.first << c.second << c.result;
}

class QBsdKeyboardHandler : public QObject
{
    Q_OBJECT
public:
    QBsdKeyboardHandler(const QString &key, const QString &specification);
    ~QBsdKeyboardHandler();

    enum KeycodeAction {
        None               = 0,

        CapsLockOff        = 0x01000000,
        CapsLockOn         = 0x01000001,
        NumLockOff         = 0x02000000,
        NumLockOn          = 0x02000001,
        ScrollLockOff      = 0x03000000,
        ScrollLockOn       = 0x03000001,

        Reboot             = 0x04000000,

        PreviousConsole    = 0x05000000,
        NextConsole        = 0x05000001,
        SwitchConsoleFirst = 0x06000000,
        SwitchConsoleLast  = 0x0600007f,
        SwitchConsoleMask  = 0x0000007f
    };

    static Qt::KeyboardModifiers toQtModifiers(quint8 mod)
    {
        Qt::KeyboardModifiers qtmod = Qt::NoModifier;

        if (mod & (QBsdKeyboardMap::ModShift | QBsdKeyboardMap::ModShiftL | QBsdKeyboardMap::ModShiftR))
            qtmod |= Qt::ShiftModifier;
        if (mod & (QBsdKeyboardMap::ModControl | QBsdKeyboardMap::ModCtrlL | QBsdKeyboardMap::ModCtrlR))
            qtmod |= Qt::ControlModifier;
        if (mod & QBsdKeyboardMap::ModAlt)
            qtmod |= Qt::AltModifier;

        return qtmod;
    }



protected:
    bool loadKeymap(const QString &file);
    void unloadKeymap();
    void switchLed(int led, bool state);
    KeycodeAction processKeycode(quint16 keycode, bool pressed, bool autorepeat);
    void processKeyEvent(int nativecode, int unicode, int qtcode,
                         Qt::KeyboardModifiers modifiers, bool isPress, bool autoRepeat);\
    void revertTTYSettings();

private slots:
    void readKeyboardData();

private:
    QSocketNotifier * m_notify;
    struct termios *m_kbd_orig_tty;
    int m_fd;
    bool m_should_close;
    QString m_spec;

    // keymap handling
    quint8 m_modifiers;
    quint8 m_locks[3];
    int m_composing;
    quint16 m_dead_unicode;

    bool m_no_zap;
    bool m_do_compose;

    const QBsdKeyboardMap::Mapping *m_keymap;
    int m_keymap_size;
    const QBsdKeyboardMap::Composing *m_keycompose;
    int m_keycompose_size;

    int m_orig_kbd_mode;

    static const QBsdKeyboardMap::Mapping s_keymap_default[];
    static const QBsdKeyboardMap::Composing s_keycompose_default[];

};

QT_END_NAMESPACE

#endif // QTSLIB_H
