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


#include "qbsdsysmouse.h"


#include <QSocketNotifier>
#include <QStringList>
#include <QPoint>
#include <QGuiApplication>
#include <qpa/qwindowsysteminterface.h>

#include <errno.h>

#include <qdebug.h>

#include <sys/mouse.h>
#include <fcntl.h>
#include <unistd.h>

QT_BEGIN_NAMESPACE

QBsdSysMouseHandler::QBsdSysMouseHandler(const QString &key,
                                                 const QString &specification)
    : m_notify(0), m_packet_size(0), m_x(0), m_y(0), m_xoffset(0), m_yoffset(0), m_buttons(Qt::NoButton)
{
    QString device;
    int level;

    // qDebug() << "QBsdSysMouseHandler" << key << specification;
    setObjectName(QLatin1String("BSD Sysmouse Handler"));

    if (specification.startsWith("/dev/"))
        device = specification.toLocal8Bit();

    if (device.isEmpty())
        device = QByteArrayLiteral("/dev/sysmouse");

    m_dev_fd = open(device.toLatin1(), O_RDONLY);
    if (m_dev_fd < 0) {
        qErrnoWarning(errno, "open(%s) failed", (const char*)device.toLatin1());
        return;
    }

    if (::ioctl(m_dev_fd, MOUSE_GETLEVEL, &level)) {
        qErrnoWarning(errno, "ioctl(%s, MOUSE_GETLEVEL) failed", (const char*)device.toLatin1());
        m_packet_size = 5;
    }
    else {
        if (level)
            m_packet_size = 8;
        else
            m_packet_size = 5;
    }

    if (fcntl(m_dev_fd, F_SETFL, O_NONBLOCK)) {
        qErrnoWarning(errno, "fcntl(%s, F_SETFL, O_NONBLOCK) failed", (const char*)device.toLatin1());
    }

    if (m_dev_fd >= 0) {
        m_notify = new QSocketNotifier(m_dev_fd, QSocketNotifier::Read, this);
        connect(m_notify, SIGNAL(activated(int)), this, SLOT(readMouseData()));
    } else {
        qWarning("Cannot open mouse input device '%s': %s", (const char*)device.toLatin1(), strerror(errno));
    }
}


QBsdSysMouseHandler::~QBsdSysMouseHandler()
{
    if (m_dev_fd != -1)
        close(m_dev_fd);
}


void QBsdSysMouseHandler::readMouseData()
{
    int8_t packet[MOUSE_SYS_PACKETSIZE];
    uint8_t status;
    int16_t relx, rely;
    int bytes;

    if (m_dev_fd < 0)
        return;

    if (m_packet_size == 0)
        return;

    while ((bytes = read(m_dev_fd, packet, m_packet_size)) == m_packet_size) {
        relx = packet[1] + packet[3];
        rely = -(packet[2] + packet[4]);

        m_x += relx;
        m_y += rely;

        status = packet[0] & MOUSE_SYS_STDBUTTONS;
    }

    // clamp to screen geometry
    QRect g = QGuiApplication::primaryScreen()->virtualGeometry();
    if (m_x + m_xoffset < g.left())
        m_x = g.left() - m_xoffset;
    else if (m_x + m_xoffset > g.right())
        m_x = g.right() - m_xoffset;

    if (m_y + m_yoffset < g.top())
        m_y = g.top() - m_yoffset;
    else if (m_y + m_yoffset > g.bottom())
        m_y = g.bottom() - m_yoffset;

    QPoint pos(m_x + m_xoffset, m_y + m_yoffset);
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
