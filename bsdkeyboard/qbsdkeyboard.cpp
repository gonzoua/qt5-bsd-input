/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Copyright (C) 2015-2016 Oleksandr Tymoshenko <gonzo@bluezbox.com>
** Contact: http://www.qt.io/licensing/
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

#include <errno.h>

#include <qdebug.h>

#include <fcntl.h>
#include <unistd.h>

#include <termios.h>
#include <sys/kbio.h>

// #define QT_QPA_KEYMAP_DEBUG

#ifdef QT_QPA_KEYMAP_DEBUG
#include <qdebug.h>
#endif

QT_BEGIN_NAMESPACE

#include "qbsdkeyboard_defaultmap.h"

QBsdKeyboardHandler::QBsdKeyboardHandler(const QString &key,
                                                 const QString &specification)
    : m_notify(0), m_kbd_orig_tty(0), m_should_close(false),
      m_modifiers(0), m_composing(0), m_dead_unicode(0xffff),
      m_no_zap(true), m_do_compose(false),
      m_keymap(0), m_keymap_size(0), m_keycompose(0), m_keycompose_size(0)

{
    Q_UNUSED(key);
    QString device;
    QString keymapFile;

    // qDebug() << "QBsdKeyboardHandler" << key << specification;
    setObjectName(QLatin1String("BSD Keyboard Handler"));

    if (specification.startsWith("/dev/"))
        device = specification.toLocal8Bit();

    if (device.isEmpty()) {
        device = QByteArrayLiteral("STDIN");
        m_fd = fileno(stdin);
    } 
    else {
        m_fd = open(device.toLatin1(), O_RDONLY);
        if (!m_fd) {
            qErrnoWarning(errno, "open(%s) failed", (const char*)device.toLatin1());
            return;
        }
        m_should_close = true;
    }

    if (::ioctl(m_fd, KDGKBMODE, &m_orig_kbd_mode)) {
        qErrnoWarning(errno, "ioctl(%s, KDGKBMODE) failed", (const char*)device.toLatin1());
        revertTTYSettings();
        return;
    }

    if (::ioctl(m_fd, KDSKBMODE, K_CODE) < 0) {
        qErrnoWarning(errno, "ioctl(%s, KDSKBMODE) failed", (const char*)device.toLatin1());
        revertTTYSettings();
        return;
    }

    struct termios kbdtty;
    if (tcgetattr(m_fd, &kbdtty) == 0) {

        m_kbd_orig_tty = new struct termios;
        *m_kbd_orig_tty = kbdtty;

        kbdtty.c_iflag = IGNPAR | IGNBRK;
        kbdtty.c_oflag = 0;
        kbdtty.c_cflag = CREAD | CS8;
        kbdtty.c_lflag = 0;
        kbdtty.c_cc[VTIME] = 0;
        kbdtty.c_cc[VMIN] = 1;
        cfsetispeed(&kbdtty, 9600);
        cfsetospeed(&kbdtty, 9600);
        if (tcsetattr(m_fd, TCSANOW, &kbdtty) < 0) {
            qErrnoWarning(errno, "tcsetattr(%s) failed", (const char*)device.toLatin1());
            revertTTYSettings();
            return;
        }
    } else {
        qErrnoWarning(errno, "tcgetattr(%s) failed", (const char*)device.toLatin1());
        revertTTYSettings();
        return;
    }

    if (fcntl(m_fd, F_SETFL, O_NONBLOCK)) {
        qErrnoWarning(errno, "fcntl(%s, F_SETFL, O_NONBLOCK) failed", (const char*)device.toLatin1());
        revertTTYSettings();
        return;
    }

    if (keymapFile.isEmpty() || !loadKeymap(keymapFile))
        unloadKeymap();

    if (m_fd >= 0) {
        m_notify = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
        connect(m_notify, SIGNAL(activated(int)), this, SLOT(readKeyboardData()));
    } else {
        qWarning("Cannot open keyboard input device '%s': %s", (const char*)device.toLatin1(), strerror(errno));
    }
}

QBsdKeyboardHandler::~QBsdKeyboardHandler()
{
    revertTTYSettings();
}

void QBsdKeyboardHandler::revertTTYSettings()
{
    if (m_fd >= 0) {
        if (m_kbd_orig_tty != 0) {
            ::tcsetattr(m_fd, TCSANOW, m_kbd_orig_tty);
            delete m_kbd_orig_tty;
            m_kbd_orig_tty = 0;
        }

        ::ioctl(m_fd, KDSKBMODE, m_orig_kbd_mode);
        if (m_should_close)
            close(m_fd);
    }
}

void QBsdKeyboardHandler::readKeyboardData()
{
    uint8_t buffer[32];

    forever {
        int result = ::read(m_fd, buffer, sizeof(buffer));

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
            quint16 code = buffer[i] & 0x7f;
            bool pressed = (buffer[i] & 0x80) ? false : true;

            QBsdKeyboardHandler::KeycodeAction ka;
            ka = processKeycode(code, pressed, false);

            switch (ka) {
            case QBsdKeyboardHandler::CapsLockOn:
            case QBsdKeyboardHandler::CapsLockOff:
                switchLed(LED_CAP, ka == QBsdKeyboardHandler::CapsLockOn);
                break;

            case QBsdKeyboardHandler::NumLockOn:
            case QBsdKeyboardHandler::NumLockOff:
                switchLed(LED_NUM, ka == QBsdKeyboardHandler::NumLockOn);
                break;

            case QBsdKeyboardHandler::ScrollLockOn:
            case QBsdKeyboardHandler::ScrollLockOff:
                switchLed(LED_SCR, ka == QBsdKeyboardHandler::ScrollLockOn);
                break;

            default:
                // ignore console switching and reboot
                break;
            }
        }
    }
}

void QBsdKeyboardHandler::processKeyEvent(int nativecode, int unicode, int qtcode,
                                            Qt::KeyboardModifiers modifiers, bool isPress, bool autoRepeat)
{
    QWindowSystemInterface::handleExtendedKeyEvent(0, (isPress ? QEvent::KeyPress : QEvent::KeyRelease),
                                                   qtcode, modifiers, nativecode + 8, 0, int(modifiers),
                                                   (unicode != 0xffff ) ? QString(unicode) : QString(), autoRepeat);
}

QBsdKeyboardHandler::KeycodeAction QBsdKeyboardHandler::processKeycode(quint16 keycode, bool pressed, bool autorepeat)
{
    KeycodeAction result = None;
    bool first_press = pressed && !autorepeat;

    const QBsdKeyboardMap::Mapping *map_plain = 0;
    const QBsdKeyboardMap::Mapping *map_withmod = 0;

    quint8 modifiers = m_modifiers;

    // get a specific and plain mapping for the keycode and the current modifiers
    for (int i = 0; i < m_keymap_size && !(map_plain && map_withmod); ++i) {
        const QBsdKeyboardMap::Mapping *m = m_keymap + i;
        if (m->keycode == keycode) {
            if (m->modifiers == 0)
                map_plain = m;

            quint8 testmods = m_modifiers;
            if (m_locks[0] /*CapsLock*/ && (m->flags & QBsdKeyboardMap::IsLetter))
                testmods ^= QBsdKeyboardMap::ModShift;
            if (m->modifiers == testmods)
                map_withmod = m;
        }
    }

    if (m_locks[0] /*CapsLock*/ && map_withmod && (map_withmod->flags & QBsdKeyboardMap::IsLetter))
        modifiers ^= QBsdKeyboardMap::ModShift;

#ifdef QT_QPA_KEYMAP_DEBUG
    qWarning("Processing key event: keycode=%3d, modifiers=%02x pressed=%d, autorepeat=%d  |  plain=%d, withmod=%d, size=%d", \
             keycode, modifiers, pressed ? 1 : 0, autorepeat ? 1 : 0, \
             map_plain ? map_plain - m_keymap : -1, \
             map_withmod ? map_withmod - m_keymap : -1, \
             m_keymap_size);
#endif

    const QBsdKeyboardMap::Mapping *it = map_withmod ? map_withmod : map_plain;

    if (!it) {
#ifdef QT_QPA_KEYMAP_DEBUG
        // we couldn't even find a plain mapping
        qWarning("Could not find a suitable mapping for keycode: %3d, modifiers: %02x", keycode, modifiers);
#endif
        return result;
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
            quint8 &lock = m_locks[qtcode - Qt::Key_CapsLock];
            lock ^= 1;

            switch (qtcode) {
            case Qt::Key_CapsLock  : result = lock ? CapsLockOn : CapsLockOff; break;
            case Qt::Key_NumLock   : result = lock ? NumLockOn : NumLockOff; break;
            case Qt::Key_ScrollLock: result = lock ? ScrollLockOn : ScrollLockOff; break;
            default                : break;
            }
        }
    } else if ((it->flags & QBsdKeyboardMap::IsSystem) && it->special && first_press) {
        switch (it->special) {
        case QBsdKeyboardMap::SystemReboot:
            result = Reboot;
            break;

        case QBsdKeyboardMap::SystemZap:
            if (!m_no_zap)
                qApp->quit();
            break;

        case QBsdKeyboardMap::SystemConsolePrevious:
            result = PreviousConsole;
            break;

        case QBsdKeyboardMap::SystemConsoleNext:
            result = NextConsole;
            break;

        default:
            if (it->special >= QBsdKeyboardMap::SystemConsoleFirst &&
                it->special <= QBsdKeyboardMap::SystemConsoleLast) {
                result = KeycodeAction(SwitchConsoleFirst + ((it->special & QBsdKeyboardMap::SystemConsoleMask) & SwitchConsoleMask));
            }
            break;
        }

        skip = true; // no need to tell Qt about it
    } else if ((qtcode == Qt::Key_Multi_key) && m_do_compose) {
        // the Compose key was pressed
        if (first_press)
            m_composing = 2;
        skip = true;
    } else if ((it->flags & QBsdKeyboardMap::IsDead) && m_do_compose) {
        // a Dead key was pressed
        if (first_press && m_composing == 1 && m_dead_unicode == unicode) { // twice
            m_composing = 0;
            qtcode = Qt::Key_unknown; // otherwise it would be Qt::Key_Dead...
        } else if (first_press && unicode != 0xffff) {
            m_dead_unicode = unicode;
            m_composing = 1;
            skip = true;
        } else {
            skip = true;
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

        if (m_composing == 2 && first_press && !(it->flags & QBsdKeyboardMap::IsModifier)) {
            // the last key press was the Compose key
            if (unicode != 0xffff) {
                int idx = 0;
                // check if this code is in the compose table at all
                for ( ; idx < m_keycompose_size; ++idx) {
                    if (m_keycompose[idx].first == unicode)
                        break;
                }
                if (idx < m_keycompose_size) {
                    // found it -> simulate a Dead key press
                    m_dead_unicode = unicode;
                    unicode = 0xffff;
                    m_composing = 1;
                    skip = true;
                } else {
                    m_composing = 0;
                }
            } else {
                m_composing = 0;
            }
        } else if (m_composing == 1 && first_press && !(it->flags & QBsdKeyboardMap::IsModifier)) {
            // the last key press was a Dead key
            bool valid = false;
            if (unicode != 0xffff) {
                int idx = 0;
                // check if this code is in the compose table at all
                for ( ; idx < m_keycompose_size; ++idx) {
                    if (m_keycompose[idx].first == m_dead_unicode && m_keycompose[idx].second == unicode)
                        break;
                }
                if (idx < m_keycompose_size) {
                    quint16 composed = m_keycompose[idx].result;
                    if (composed != 0xffff) {
                        unicode = composed;
                        qtcode = Qt::Key_unknown;
                        valid = true;
                    }
                }
            }
            if (!valid) {
                unicode = m_dead_unicode;
                qtcode = Qt::Key_unknown;
            }
            m_composing = 0;
        }

        if (!skip) {
#ifdef QT_QPA_KEYMAP_DEBUG
            qWarning("Processing: uni=%04x, qt=%08x, qtmod=%08x", unicode, qtcode & ~modmask, (qtcode & modmask));
#endif
            //If NumLockOff and keypad key pressed remap event sent
            if (!m_locks[1] &&
                 (qtcode & Qt::KeypadModifier) &&
                 keycode >= 71 &&
                 keycode <= 83 &&
                 keycode != 74 &&
                 keycode != 78) {

                unicode = 0xffff;
                int oldMask = (qtcode & modmask);
                switch (keycode) {
                case 71: //7 --> Home
                    qtcode = Qt::Key_Home;
                    break;
                case 72: //8 --> Up
                    qtcode = Qt::Key_Up;
                    break;
                case 73: //9 --> PgUp
                    qtcode = Qt::Key_PageUp;
                    break;
                case 75: //4 --> Left
                    qtcode = Qt::Key_Left;
                    break;
                case 76: //5 --> Clear
                    qtcode = Qt::Key_Clear;
                    break;
                case 77: //6 --> right
                    qtcode = Qt::Key_Right;
                    break;
                case 79: //1 --> End
                    qtcode = Qt::Key_End;
                    break;
                case 80: //2 --> Down
                    qtcode = Qt::Key_Down;
                    break;
                case 81: //3 --> PgDn
                    qtcode = Qt::Key_PageDown;
                    break;
                case 82: //0 --> Ins
                    qtcode = Qt::Key_Insert;
                    break;
                case 83: //, --> Del
                    qtcode = Qt::Key_Delete;
                    break;
                }
                qtcode ^= oldMask;
            }

            // send the result to the server
            processKeyEvent(keycode, unicode, qtcode & ~modmask, Qt::KeyboardModifiers(qtcode & modmask), pressed, autorepeat);
        }
    }
    return result;
}

void QBsdKeyboardHandler::switchLed(int led, bool state)
{
#ifdef QT_QPA_KEYMAP_DEBUG
    qWarning() << "switchLed" << led << state;
#endif
    int leds = 0;
    if (::ioctl(m_fd, KDGETLED, &leds) < 0) {
        qWarning("switchLed: Failed to query led states.");
        return;
    }

    if (state)
        leds |= led;
    else
        leds &= ~led;

    if (::ioctl(m_fd, KDSETLED, leds) < 0) {
        qWarning("switchLed: Failed to set led states.");
        return;
    }
}

void QBsdKeyboardHandler::unloadKeymap()
{
#ifdef QT_QPA_KEYMAP_DEBUG
    qWarning() << "Unload current keymap and restore built-in";
#endif

    if (m_keymap && m_keymap != s_keymap_default)
        delete [] m_keymap;
    if (m_keycompose && m_keycompose != s_keycompose_default)
        delete [] m_keycompose;

    m_keymap = s_keymap_default;
    m_keymap_size = sizeof(s_keymap_default) / sizeof(s_keymap_default[0]);
    m_keycompose = s_keycompose_default;
    m_keycompose_size = sizeof(s_keycompose_default) / sizeof(s_keycompose_default[0]);

    // reset state, so we could switch keymaps at runtime
    m_modifiers = 0;
    memset(m_locks, 0, sizeof(m_locks));
    m_composing = 0;
    m_dead_unicode = 0xffff;

    //Set locks according to keyboard leds
    int leds = 0;
    if (::ioctl(m_fd, KDGETLED, &leds) < 0) {
        qWarning("Failed to query led states. Settings numlock & capslock off");
        switchLed(LED_NUM, false);
        switchLed(LED_CAP, false);
        switchLed(LED_SCR, false);
    } else {
        //Capslock
        if ((leds & LED_CAP) > 0)
            m_locks[0] = 1;
        //Numlock
        if ((leds & LED_NUM) > 0)
            m_locks[1] = 1;
        //Scrollock
        if ((leds & LED_SCR) > 0)
            m_locks[2] = 1;
#ifdef QT_QPA_KEYMAP_DEBUG
        qWarning("numlock=%d , capslock=%d, scrolllock=%d",m_locks[1],m_locks[0],m_locks[2]);
#endif
    }
}

bool QBsdKeyboardHandler::loadKeymap(const QString &file)
{
#ifdef QT_QPA_KEYMAP_DEBUG
    qWarning() << "Load keymap" << file;
#endif

    QFile f(file);

    if (!f.open(QIODevice::ReadOnly)) {
        qWarning("Could not open keymap file '%s'", qPrintable(file));
        return false;
    }

    // .qmap files have a very simple structure:
    // quint32 magic           (QKeyboard::FileMagic)
    // quint32 version         (1)
    // quint32 keymap_size     (# of struct QKeyboard::Mappings)
    // quint32 keycompose_size (# of struct QKeyboard::Composings)
    // all QKeyboard::Mappings via QDataStream::operator(<<|>>)
    // all QKeyboard::Composings via QDataStream::operator(<<|>>)

    quint32 qmap_magic, qmap_version, qmap_keymap_size, qmap_keycompose_size;

    QDataStream ds(&f);

    ds >> qmap_magic >> qmap_version >> qmap_keymap_size >> qmap_keycompose_size;

    if (ds.status() != QDataStream::Ok || qmap_magic != QBsdKeyboardMap::FileMagic || qmap_version != 1 || qmap_keymap_size == 0) {
        qWarning("'%s' is ot a valid.qmap keymap file.", qPrintable(file));
        return false;
    }

    QBsdKeyboardMap::Mapping *qmap_keymap = new QBsdKeyboardMap::Mapping[qmap_keymap_size];
    QBsdKeyboardMap::Composing *qmap_keycompose = qmap_keycompose_size ? new QBsdKeyboardMap::Composing[qmap_keycompose_size] : 0;

    for (quint32 i = 0; i < qmap_keymap_size; ++i)
        ds >> qmap_keymap[i];
    for (quint32 i = 0; i < qmap_keycompose_size; ++i)
        ds >> qmap_keycompose[i];

    if (ds.status() != QDataStream::Ok) {
        delete [] qmap_keymap;
        delete [] qmap_keycompose;

        qWarning("Keymap file '%s' can not be loaded.", qPrintable(file));
        return false;
    }

    // unload currently active and clear state
    unloadKeymap();

    m_keymap = qmap_keymap;
    m_keymap_size = qmap_keymap_size;
    m_keycompose = qmap_keycompose;
    m_keycompose_size = qmap_keycompose_size;

    m_do_compose = true;

    return true;

}

QT_END_NAMESPACE
