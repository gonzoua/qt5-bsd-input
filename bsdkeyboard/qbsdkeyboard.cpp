/****************************************************************************
**
** Copyright (C) 2015-2016 Oleksandr Tymoshenko <gonzo@bluezbox.com>
**
** This file is part of the QtGui module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL21$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
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
** As a special exception, The Qt Company gives you certain additional
** rights. These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qbsdkeyboard.h"

#include <QSocketNotifier>
#include <QStringList>
#include <QPoint>
#include <QGuiApplication>
#include <qpa/qwindowsysteminterface.h>
#include <private/qcore_unix_p.h>

#include <qdebug.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <termios.h>
#include <sys/kbio.h>

// #define QT_BSD_KEYBOARD_DEBUG

#ifdef QT_BSD_KEYBOARD_DEBUG
#include <qdebug.h>
#endif

QT_BEGIN_NAMESPACE

enum {
    Bsd_KeyCodeMask     = 0x7f,
    Bsd_KeyPressedMask  = 0x80
};

#include "qbsdkeyboard_defaultmap.h"

QBsdKeyboardHandler::QBsdKeyboardHandler(const QString &key,
                                                 const QString &specification) :
    m_kbdOrigTty(0),
    m_shouldClose(false),
    m_modifiers(0),
    m_keymap(0),
    m_keymapSize(0)
{
    Q_UNUSED(key);
    QByteArray device;

    setObjectName(QLatin1String("BSD Keyboard Handler"));

    if (specification.startsWith("/dev/"))
        device = QFile::encodeName(specification);

    if (device.isEmpty()) {
        device = QByteArrayLiteral("STDIN");
        m_fd = fileno(stdin);
    }
    else {
        m_fd = QT_OPEN(device.constData(), O_RDONLY);
        if (!m_fd) {
            qErrnoWarning(errno, "open(%s) failed", device.constData());
            return;
        }
        m_shouldClose = true;
    }

    if (ioctl(m_fd, KDGKBMODE, &m_origKbdMode)) {
        qErrnoWarning(errno, "ioctl(%s, KDGKBMODE) failed", device.constData());
        revertTTYSettings();
        return;
    }

    if (ioctl(m_fd, KDSKBMODE, K_CODE) < 0) {
        qErrnoWarning(errno, "ioctl(%s, KDSKBMODE) failed", device.constData());
        revertTTYSettings();
        return;
    }

    struct termios kbdtty;
    if (tcgetattr(m_fd, &kbdtty) == 0) {

        m_kbdOrigTty = new struct termios;
        *m_kbdOrigTty = kbdtty;

        kbdtty.c_iflag = IGNPAR | IGNBRK;
        kbdtty.c_oflag = 0;
        kbdtty.c_cflag = CREAD | CS8;
        kbdtty.c_lflag = 0;
        kbdtty.c_cc[VTIME] = 0;
        kbdtty.c_cc[VMIN] = 1;
        cfsetispeed(&kbdtty, 9600);
        cfsetospeed(&kbdtty, 9600);
        if (tcsetattr(m_fd, TCSANOW, &kbdtty) < 0) {
            qErrnoWarning(errno, "tcsetattr(%s) failed", device.constData());
            revertTTYSettings();
            return;
        }
    } else {
        qErrnoWarning(errno, "tcgetattr(%s) failed", device.constData());
        revertTTYSettings();
        return;
    }

    if (fcntl(m_fd, F_SETFL, O_NONBLOCK)) {
        qErrnoWarning(errno, "fcntl(%s, F_SETFL, O_NONBLOCK) failed", device.constData());
        revertTTYSettings();
        return;
    }

    resetKeymap();

    m_notifier.reset(new QSocketNotifier(m_fd, QSocketNotifier::Read, this));
    connect(m_notifier.data(), SIGNAL(activated(int)), this, SLOT(readKeyboardData()));
}

QBsdKeyboardHandler::~QBsdKeyboardHandler()
{
    revertTTYSettings();
}

void QBsdKeyboardHandler::revertTTYSettings()
{
    if (m_fd >= 0) {
        if (m_kbdOrigTty != 0) {
            tcsetattr(m_fd, TCSANOW, m_kbdOrigTty);
            delete m_kbdOrigTty;
            m_kbdOrigTty = 0;
        }

        ioctl(m_fd, KDSKBMODE, m_origKbdMode);
        if (m_shouldClose)
            close(m_fd);
        m_fd = -1;
    }
}

void QBsdKeyboardHandler::readKeyboardData()
{
    uint8_t buffer[32];

    forever {
        int result = read(m_fd, buffer, sizeof(buffer));

        if (result == 0) {
            qWarning("Got EOF from the input device.");
            return;
        } else if (result < 0) {
            if (errno != EINTR && errno != EAGAIN) {
                qWarning("Could not read from input device: %s", strerror(errno));
                return;
            }
            else
                break;
        }

        for (int i = 0; i < result; ++i) {
            quint16 code = buffer[i] & Bsd_KeyCodeMask;
            bool pressed = (buffer[i] & Bsd_KeyPressedMask) ? false : true;

            processKeycode(code, pressed, false);
        }
    }
}

void QBsdKeyboardHandler::processKeyEvent(int nativecode, int unicode, int qtcode,
                                            Qt::KeyboardModifiers modifiers, bool isPress, bool autoRepeat)
{
    QWindowSystemInterface::handleExtendedKeyEvent(0, (isPress ? QEvent::KeyPress : QEvent::KeyRelease),
                                                   qtcode, modifiers, nativecode, 0, int(modifiers),
                                                   (unicode != 0xffff ) ? QString(unicode) : QString(), autoRepeat);
}

void QBsdKeyboardHandler::processKeycode(quint16 keycode, bool pressed, bool autorepeat)
{
    bool first_press = pressed && !autorepeat;

    const QBsdKeyboardMap::Mapping *map_plain = 0;
    const QBsdKeyboardMap::Mapping *map_withmod = 0;

    quint8 modifiers = m_modifiers;

    // get a specific and plain mapping for the keycode and the current modifiers
    for (int i = 0; i < m_keymapSize && !(map_plain && map_withmod); ++i) {
        const QBsdKeyboardMap::Mapping *m = m_keymap + i;
        if (m->keycode == keycode) {
            if (m->modifiers == 0)
                map_plain = m;

            quint8 testmods = m_modifiers;
            if (m_capsLock && (m->flags & QBsdKeyboardMap::IsLetter))
                testmods ^= QBsdKeyboardMap::ModShift;
            if (m->modifiers == testmods)
                map_withmod = m;
        }
    }

    if (m_capsLock && map_withmod && (map_withmod->flags & QBsdKeyboardMap::IsLetter))
        modifiers ^= QBsdKeyboardMap::ModShift;

#ifdef QT_BSD_KEYBOARD_DEBUG
    qWarning("Processing key event: keycode=%3d, modifiers=%02x pressed=%d, autorepeat=%d  |  plain=%jd, withmod=%jd, size=%d", \
             keycode, modifiers, pressed ? 1 : 0, autorepeat ? 1 : 0, \
             map_plain ? (uintptr_t)(map_plain - m_keymap) : -1, \
             map_withmod ? (uintptr_t)(map_withmod - m_keymap) : -1, \
             m_keymapSize);
#endif

    const QBsdKeyboardMap::Mapping *it = map_withmod ? map_withmod : map_plain;

    if (!it) {
#ifdef QT_BSD_KEYBOARD_DEBUG
        // we couldn't even find a plain mapping
        qWarning("Could not find a suitable mapping for keycode: %3d, modifiers: %02x", keycode, modifiers);
#endif
        return;
    }

    bool skip = false;
    quint16 unicode = it->unicode;
    quint32 qtcode = it->qtcode;

    if ((it->flags & QBsdKeyboardMap::IsModifier) && it->special) {
        // this is a modifier, i.e. Shift, Alt, ...
        if (pressed)
            m_modifiers |= quint8(it->special);
        else
            m_modifiers &= ~quint8(it->special);
    } else if (qtcode >= Qt::Key_CapsLock && qtcode <= Qt::Key_ScrollLock) {
        // (Caps|Num|Scroll)Lock
        if (first_press) {
            switch (qtcode) {
            case Qt::Key_CapsLock:
                m_capsLock = !m_capsLock;
                switchLed(LED_CAP, m_capsLock);
                break;
            case Qt::Key_NumLock:
                m_numLock = !m_numLock;
                switchLed(LED_NUM, m_numLock);
                break;
            case Qt::Key_ScrollLock:
                m_scrollLock = !m_scrollLock;
                switchLed(LED_SCR, m_scrollLock);
                break;
            default:
                break;
            }
        }
    }

    if (!skip) {
        // a normal key was pressed
        const int modmask = Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier | Qt::KeypadModifier;

        // we couldn't find a specific mapping for the current modifiers,
        // or that mapping didn't have special modifiers:
        // so just report the plain mapping with additional modifiers.
        if ((it == map_plain && it != map_withmod) ||
            (map_withmod && !(map_withmod->qtcode & modmask))) {
            qtcode |= QBsdKeyboardHandler::toQtModifiers(modifiers);
        }

#ifdef QT_BSD_KEYBOARD_DEBUG
        qWarning("Processing: uni=%04x, qt=%08x, qtmod=%08x", unicode, qtcode & ~modmask, (qtcode & modmask));
#endif
        //If NumLockOff and keypad key pressed remap event sent
        if (!m_numLock &&
             (qtcode & Qt::KeypadModifier)) {
            unicode = 0xffff;
            int oldMask = (qtcode & modmask);
            switch (qtcode & ~modmask) {
            case Qt::Key_7: //7 --> Home
                qtcode = Qt::Key_Home;
                break;
            case Qt::Key_8: //8 --> Up
                qtcode = Qt::Key_Up;
                break;
            case Qt::Key_9: //9 --> PgUp
                qtcode = Qt::Key_PageUp;
                break;
            case Qt::Key_4: //4 --> Left
                qtcode = Qt::Key_Left;
                break;
            case Qt::Key_5: //5 --> Clear
                qtcode = Qt::Key_Clear;
                break;
            case Qt::Key_6: //6 --> right
                qtcode = Qt::Key_Right;
                break;
            case Qt::Key_1: //1 --> End
                qtcode = Qt::Key_End;
                break;
            case Qt::Key_2: //2 --> Down
                qtcode = Qt::Key_Down;
                break;
            case Qt::Key_3: //3 --> PgDn
                qtcode = Qt::Key_PageDown;
                break;
            case Qt::Key_0: //0 --> Ins
                qtcode = Qt::Key_Insert;
                break;
            case Qt::Key_Period: //. --> Del
                qtcode = Qt::Key_Delete;
                break;
            }
            qtcode |= oldMask;
        }

        // send the result to the server
        processKeyEvent(keycode, unicode, qtcode & ~modmask, Qt::KeyboardModifiers(qtcode & modmask), pressed, autorepeat);
    }
}

void QBsdKeyboardHandler::switchLed(int led, bool state)
{
#ifdef QT_BSD_KEYBOARD_DEBUG
    qWarning() << "switchLed" << led << state;
#endif
    int leds = 0;
    if (ioctl(m_fd, KDGETLED, &leds) < 0) {
        qWarning("switchLed: Failed to query led states.");
        return;
    }

    if (state)
        leds |= led;
    else
        leds &= ~led;

    if (ioctl(m_fd, KDSETLED, leds) < 0) {
        qWarning("switchLed: Failed to set led states.");
        return;
    }
}

void QBsdKeyboardHandler::resetKeymap()
{
#ifdef QT_BSD_KEYBOARD_DEBUG
    qWarning() << "Unload current keymap and restore built-in";
#endif

    if (m_keymap && m_keymap != s_keymapDefault)
        delete [] m_keymap;

    m_keymap = s_keymapDefault;
    m_keymapSize = sizeof(s_keymapDefault) / sizeof(s_keymapDefault[0]);

    // reset state, so we could switch keymaps at runtime
    m_modifiers = 0;
    m_capsLock = false;
    m_numLock = false;
    m_scrollLock = false;

    //Set locks according to keyboard leds
    int leds = 0;
    if (ioctl(m_fd, KDGETLED, &leds) < 0) {
        qWarning("Failed to query led states. Settings numlock & capslock off");
        switchLed(LED_NUM, false);
        switchLed(LED_CAP, false);
        switchLed(LED_SCR, false);
    } else {
        if ((leds & LED_CAP) > 0)
            m_capsLock = true;
        if ((leds & LED_NUM) > 0)
            m_numLock = true;
        if ((leds & LED_SCR) > 0)
            m_scrollLock = true;
#ifdef QT_BSD_KEYBOARD_DEBUG
        qWarning("numlock=%d , capslock=%d, scrolllock=%d",m_numLock, m_capsLock, m_scrollLock);
#endif
    }
}
