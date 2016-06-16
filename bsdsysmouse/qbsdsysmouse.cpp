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

#include "qbsdsysmouse.h"

#include <QSocketNotifier>
#include <QStringList>
#include <QPoint>
#include <QGuiApplication>
#include <qpa/qwindowsysteminterface.h>

#include <private/qcore_unix_p.h>
#include <qdebug.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/mouse.h>
#include <unistd.h>

QT_BEGIN_NAMESPACE

enum {
    PsmLevelBasic = 0,
    PsmLevelExtended = 1,
    PsmLevelNative = 2
};

QBsdSysMouseHandler::QBsdSysMouseHandler(const QString &key, const QString &specification) :
    m_notifier(0),
    m_packetSize(0),
    m_x(0),
    m_y(0),
    m_xOffset(0),
    m_yOffset(0),
    m_buttons(Qt::NoButton)
{
    QString device;
    int level;
    Q_UNUSED(key);

    setObjectName(QLatin1String("BSD Sysmouse Handler"));

    if (specification.startsWith("/dev/"))
        device = specification.toLocal8Bit();

    if (device.isEmpty())
        device = QByteArrayLiteral("/dev/sysmouse");

    const QByteArray devPath = QFile::encodeName(device);
    m_devFd = QT_OPEN(devPath.constData(), O_RDONLY);
    if (m_devFd < 0) {
        qErrnoWarning(errno, "open(%s) failed", devPath.constData());
        return;
    }

    if (ioctl(m_devFd, MOUSE_GETLEVEL, &level)) {
        qErrnoWarning(errno, "ioctl(%s, MOUSE_GETLEVEL) failed", devPath.constData());
        close(m_devFd);
        m_devFd = -1;
        return;
    }

    switch (level) {
    case PsmLevelBasic:
        m_packetSize = 5;
        break;
    case PsmLevelExtended:
        m_packetSize = 8;
        break;
    default:
        qWarning("Unsupported mouse device operation level: %d", level);
        close(m_devFd);
        m_devFd = -1;
        return;
    }

    if (fcntl(m_devFd, F_SETFL, O_NONBLOCK)) {
        qErrnoWarning(errno, "fcntl(%s, F_SETFL, O_NONBLOCK) failed", devPath.constData());
        close(m_devFd);
        m_devFd = -1;
        return;
    }

    m_notifier = new QSocketNotifier(m_devFd, QSocketNotifier::Read, this);
    connect(m_notifier, SIGNAL(activated(int)), this, SLOT(readMouseData()));
}

QBsdSysMouseHandler::~QBsdSysMouseHandler()
{
    if (m_devFd != -1)
        close(m_devFd);
}

void QBsdSysMouseHandler::readMouseData()
{
    int8_t packet[MOUSE_SYS_PACKETSIZE];
    uint8_t status;
    int16_t relx, rely;
    int bytes;

    if (m_devFd < 0)
        return;

    if (m_packetSize == 0)
        return;

    // packet format described in mouse(4)
    while ((bytes = read(m_devFd, packet, m_packetSize)) == m_packetSize) {
        relx = packet[1] + packet[3];
        rely = -(packet[2] + packet[4]);

        m_x += relx;
        m_y += rely;

        status = packet[0] & MOUSE_SYS_STDBUTTONS;
    }

    // clamp to screen geometry
    QRect g = QGuiApplication::primaryScreen()->virtualGeometry();
    if (m_x + m_xOffset < g.left())
        m_x = g.left() - m_xOffset;
    else if (m_x + m_xOffset > g.right())
        m_x = g.right() - m_xOffset;

    if (m_y + m_yOffset < g.top())
        m_y = g.top() - m_yOffset;
    else if (m_y + m_yOffset > g.bottom())
        m_y = g.bottom() - m_yOffset;

    QPoint pos(m_x + m_xOffset, m_y + m_yOffset);
    m_buttons = Qt::NoButton;
    if (!(status & MOUSE_SYS_BUTTON1UP))
        m_buttons |= Qt::LeftButton;
    if (!(status & MOUSE_SYS_BUTTON2UP))
        m_buttons |= Qt::MiddleButton;
    if (!(status & MOUSE_SYS_BUTTON3UP))
        m_buttons |= Qt::RightButton;

    QWindowSystemInterface::handleMouseEvent(0, pos, pos, m_buttons);
}

QT_END_NAMESPACE
