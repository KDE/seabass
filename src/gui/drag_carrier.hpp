// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QQuickItem>

namespace seabass::gui
{

// Carries a drag the pointer already started somewhere else: carry() takes
// the mouse grab from the item it began on, and from then on every move
// and the release come here, wherever that item has gone.
//
// Browse's track drag starts on a row, and a row is a list delegate: a
// playlist opened by holding the track over it (spring-loading) rebuilds
// the list, the row and its MouseArea are destroyed, and the drag they
// held died with them, the card stuck where it was. The drag card carries
// itself with this instead (TrackDragItem), so it outlives its row.
//
// carry() only works while a mouse event is being delivered (QQuickItem::
// grabMouse takes the grab of the event in delivery): call it from the
// handler of the move that started the drag.
class DragCarrier : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool carrying READ carrying NOTIFY carryingChanged)

public:
    explicit DragCarrier(QQuickItem *parent = nullptr);

    bool carrying() const { return m_carrying; }

    // Takes the mouse grab; false when there was no event to take it from.
    Q_INVOKABLE bool carry();
    // Lets go of the grab without a signal: the drag ended some other way.
    Q_INVOKABLE void putDown();

signals:
    void carryingChanged();
    // Where the pointer is, in scene coordinates.
    void moved(qreal sceneX, qreal sceneY);
    // The button was let go there.
    void released(qreal sceneX, qreal sceneY);
    // The grab was taken away (the window lost the pointer): no drop.
    void canceled();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;

private:
    void setCarrying(bool carrying);

    bool m_carrying = false;
    bool m_puttingDown = false;
};

}  // namespace seabass::gui
