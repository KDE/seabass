// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/drag_carrier.hpp"

#include <QCursor>
#include <QMouseEvent>
#include <QQuickWindow>

namespace seabass::gui
{

DragCarrier::DragCarrier(QQuickItem *parent) : QQuickItem(parent)
{
    // Takes no press of its own: it only ever receives a grab handed over.
    setAcceptedMouseButtons(Qt::NoButton);
}

bool DragCarrier::carry()
{
    if (!window()) {
        return false;
    }
    // Carrying before the grab moves: the item losing it hears of that
    // at once, inside grabMouse(), and asks whether the drag goes on.
    setCarrying(true);
    setAcceptedMouseButtons(Qt::AllButtons);
    grabMouse();
    if (window()->mouseGrabberItem() != this) {
        setAcceptedMouseButtons(Qt::NoButton);
        setCarrying(false);
        return false;
    }
    setKeepMouseGrab(true);
    // The closed hand the row showed while it held the drag.
    setCursor(QCursor(Qt::ClosedHandCursor));
    return true;
}

void DragCarrier::putDown()
{
    if (!m_carrying) {
        return;
    }
    m_puttingDown = true;
    setCarrying(false);
    setKeepMouseGrab(false);
    setAcceptedMouseButtons(Qt::NoButton);
    unsetCursor();
    if (window() && window()->mouseGrabberItem() == this) {
        ungrabMouse();
    }
    m_puttingDown = false;
}

void DragCarrier::mousePressEvent(QMouseEvent *event)
{
    event->ignore();
}

void DragCarrier::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_carrying) {
        event->ignore();
        return;
    }
    event->accept();
    emit moved(event->scenePosition().x(), event->scenePosition().y());
}

void DragCarrier::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_carrying) {
        event->ignore();
        return;
    }
    event->accept();
    const QPointF at = event->scenePosition();
    putDown();
    emit released(at.x(), at.y());
}

void DragCarrier::mouseUngrabEvent()
{
    if (!m_carrying || m_puttingDown) {
        return;
    }
    setCarrying(false);
    setKeepMouseGrab(false);
    setAcceptedMouseButtons(Qt::NoButton);
    unsetCursor();
    emit canceled();
}

void DragCarrier::setCarrying(bool carrying)
{
    if (m_carrying == carrying) {
        return;
    }
    m_carrying = carrying;
    emit carryingChanged();
}

}  // namespace seabass::gui
