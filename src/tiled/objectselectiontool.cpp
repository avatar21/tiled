/*
 * objectselectiontool.cpp
 * Copyright 2010-2013, Thorbjørn Lindeijer <thorbjorn@lindeijer.nl>
 *
 * This file is part of Tiled.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "objectselectiontool.h"

#include "changepolygon.h"
#include "layer.h"
#include "map.h"
#include "mapdocument.h"
#include "mapobject.h"
#include "mapobjectitem.h"
#include "maprenderer.h"
#include "mapscene.h"
#include "movemapobject.h"
#include "objectgroup.h"
#include "preferences.h"
#include "raiselowerhelper.h"
#include "resizemapobject.h"
#include "rotatemapobject.h"
#include "selectionrectangle.h"
#include "snaphelper.h"
#include "tile.h"
#include "tileset.h"

#include <QApplication>
#include <QGraphicsItem>
#include <QGraphicsView>
#include <QKeyEvent>
#include <QTransform>
#include <QUndoStack>

#include <cmath>

// MSVC 2010 math header does not come with M_PI
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace Tiled;
using namespace Tiled::Internal;

namespace Tiled {
namespace Internal {

enum AnchorPosition {
    TopLeftAnchor,
    TopRightAnchor,
    BottomLeftAnchor,
    BottomRightAnchor,

    TopAnchor,
    LeftAnchor,
    RightAnchor,
    BottomAnchor,

    CornerAnchorCount = 4,
    AnchorCount = 8,
};


static QPainterPath createRotateArrow()
{
    const qreal arrowHeadPos = 12;
    const qreal arrowHeadLength = 4.5;
    const qreal arrowHeadWidth = 5;
    const qreal bodyWidth = 1.5;
    const qreal outerArcSize = arrowHeadPos + bodyWidth - arrowHeadLength;
    const qreal innerArcSize = arrowHeadPos - bodyWidth - arrowHeadLength;

    QPainterPath path;
    path.moveTo(arrowHeadPos, 0);
    path.lineTo(arrowHeadPos + arrowHeadWidth, arrowHeadLength);
    path.lineTo(arrowHeadPos + bodyWidth, arrowHeadLength);
    path.arcTo(QRectF(arrowHeadLength - outerArcSize,
                      arrowHeadLength - outerArcSize,
                      outerArcSize * 2,
                      outerArcSize * 2),
               0, -90);
    path.lineTo(arrowHeadLength, arrowHeadPos + arrowHeadWidth);
    path.lineTo(0, arrowHeadPos);
    path.lineTo(arrowHeadLength, arrowHeadPos - arrowHeadWidth);
    path.lineTo(arrowHeadLength, arrowHeadPos - bodyWidth);
    path.arcTo(QRectF(arrowHeadLength - innerArcSize,
                      arrowHeadLength - innerArcSize,
                      innerArcSize * 2,
                      innerArcSize * 2),
               -90, 90);
    path.lineTo(arrowHeadPos - arrowHeadWidth, arrowHeadLength);
    path.closeSubpath();

    return path;
}

static QPainterPath createResizeArrow(bool straight)
{
    const qreal arrowLength = straight ? 14 : 16;
    const qreal arrowHeadLength = 4.5;
    const qreal arrowHeadWidth = 5;
    const qreal bodyWidth = 1.5;

    QPainterPath path;
    path.lineTo(arrowHeadWidth, arrowHeadLength);
    path.lineTo(0 + bodyWidth, arrowHeadLength);
    path.lineTo(0 + bodyWidth, arrowLength - arrowHeadLength);
    path.lineTo(arrowHeadWidth, arrowLength - arrowHeadLength);
    path.lineTo(0, arrowLength);
    path.lineTo(-arrowHeadWidth, arrowLength - arrowHeadLength);
    path.lineTo(0 - bodyWidth, arrowLength - arrowHeadLength);
    path.lineTo(0 - bodyWidth, arrowHeadLength);
    path.lineTo(-arrowHeadWidth, arrowHeadLength);
    path.closeSubpath();
    path.translate(0, straight ? 2 : 3);

    return path;
}



/**
 * Shared superclass for rotation and resizing handles.
 */
class Handle : public QGraphicsItem
{
public:
    Handle(QGraphicsItem *parent = 0)
        : QGraphicsItem(parent)
        , mUnderMouse(false)
    {
        setFlags(QGraphicsItem::ItemIgnoresTransformations |
                 QGraphicsItem::ItemIgnoresParentOpacity);
        setAcceptHoverEvents(true);
        setCursor(Qt::ArrowCursor);
    }

protected:
    void hoverEnterEvent(QGraphicsSceneHoverEvent *) { mUnderMouse = true; update(); }
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *) { mUnderMouse = false; update(); }
    QVariant itemChange(GraphicsItemChange change, const QVariant &value);

    bool mUnderMouse;
};

QVariant Handle::itemChange(GraphicsItemChange change, const QVariant &value)
{
    if (change == ItemVisibleHasChanged && value.toBool())
        if (mUnderMouse)
            mUnderMouse = isUnderMouse();
    return QGraphicsItem::itemChange(change, value);
}


/**
 * Rotation origin indicator.
 */
class OriginIndicator : public Handle
{
public:
    OriginIndicator(QGraphicsItem *parent = 0)
        : Handle(parent)
    {
        setFlag(QGraphicsItem::ItemIsMovable);
        setZValue(10000 + 1);
    }

    QRectF boundingRect() const { return QRectF(-9, -9, 18, 18); }
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *);
};

void OriginIndicator::paint(QPainter *painter,
                            const QStyleOptionGraphicsItem *,
                            QWidget *)
{
    static const QLine lines[] = {
        QLine(-8,0, 8,0),
        QLine(0,-8, 0,8),
    };
    painter->setPen(QPen(mUnderMouse ? Qt::white : Qt::lightGray, 1, Qt::DashLine));
    painter->drawLines(lines, sizeof(lines) / sizeof(lines[0]));
    painter->translate(1, 1);
    painter->setPen(QPen(Qt::black, 1, Qt::DashLine));
    painter->drawLines(lines, sizeof(lines) / sizeof(lines[0]));
}


/**
 * Corner rotation handle.
 */
class RotateHandle : public Handle
{
public:
    RotateHandle(AnchorPosition corner, QGraphicsItem *parent = 0)
        : Handle(parent)
        , mArrow(createRotateArrow())
    {
        setZValue(10000 + 1);

        QTransform transform;

        switch (corner) {
        case TopLeftAnchor:     transform.rotate(180);  break;
        case TopRightAnchor:    transform.rotate(-90);  break;
        case BottomLeftAnchor:  transform.rotate(90);   break;
        default:                break; // BottomRight
        }

        mArrow = transform.map(mArrow);
    }

    QRectF boundingRect() const { return mArrow.boundingRect().adjusted(-1, -1, 1, 1); }
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *);

private:
    QPainterPath mArrow;
};

void RotateHandle::paint(QPainter *painter, const QStyleOptionGraphicsItem *,
                         QWidget *)
{
    QPen pen(mUnderMouse ? Qt::black : Qt::lightGray, 1);
    QColor brush(mUnderMouse ? Qt::white : Qt::black);

    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(pen);
    painter->setBrush(brush);
    painter->drawPath(mArrow);
}


/**
 * A resize handle that allows resizing of map objects.
 */
class ResizeHandle : public Handle
{
public:
    ResizeHandle(AnchorPosition anchorPosition, QGraphicsItem *parent = 0)
        : Handle(parent)
        , mAnchorPosition(anchorPosition)
        , mResizingLimitHorizontal(false)
        , mResizingLimitVertical(false)
        , mArrow(createResizeArrow(anchorPosition > BottomRightAnchor))
    {
        // The bottom right anchor takes precedence
        setZValue(10000 + 1 + (anchorPosition < TopAnchor ? anchorPosition + 1 : 0));

        QTransform transform;

        switch (anchorPosition) {
        case TopLeftAnchor:     transform.rotate(135);  break;
        case TopRightAnchor:    transform.rotate(-135); break;
        case BottomLeftAnchor:  transform.rotate(45);   break;
        case BottomRightAnchor: transform.rotate(-45);  break;
        case TopAnchor:         transform.rotate(180);  mResizingLimitHorizontal = true; break;
        case LeftAnchor:        transform.rotate(90);   mResizingLimitVertical = true; break;
        case RightAnchor:       transform.rotate(-90);  mResizingLimitVertical = true; break;
        case BottomAnchor:
        default:                mResizingLimitHorizontal = true; break;
        }

        mArrow = transform.map(mArrow);
    }

    AnchorPosition anchorPosition() const { return mAnchorPosition; }
    
    void setResizingOrigin(QPointF resizingOrigin) { mResizingOrigin = resizingOrigin; }
    QPointF resizingOrigin() const { return mResizingOrigin; }
    
    bool resizingLimitHorizontal() const { return mResizingLimitHorizontal; }
    bool resizingLimitVertical() const { return mResizingLimitVertical; }
    
    QRectF boundingRect() const { return mArrow.boundingRect().adjusted(-1, -1, 1, 1); }
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *);

private:
    AnchorPosition mAnchorPosition;
    QPointF mResizingOrigin;
    bool mResizingLimitHorizontal;
    bool mResizingLimitVertical;
    QPainterPath mArrow;
};

void ResizeHandle::paint(QPainter *painter,
                         const QStyleOptionGraphicsItem *,
                         QWidget *)
{
    QPen pen(mUnderMouse ? Qt::black : Qt::lightGray, 1);
    QColor brush(mUnderMouse ? Qt::white : Qt::black);

    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(pen);
    painter->setBrush(brush);
    painter->drawPath(mArrow);
}

} // namespace Internal
} // namespace Tiled

ObjectSelectionTool::ObjectSelectionTool(QObject *parent)
    : AbstractObjectTool(tr("Select Objects"),
          QIcon(QLatin1String(":images/22x22/tool-select-objects.png")),
          QKeySequence(tr("S")),
          parent)
    , mSelectionRectangle(new SelectionRectangle)
    , mOriginIndicator(new OriginIndicator)
    , mMousePressed(false)
    , mClickedObjectItem(0)
    , mClickedRotateHandle(0)
    , mClickedResizeHandle(0)
    , mResizingLimitHorizontal(false)
    , mResizingLimitVertical(false)
    , mMode(Resize)
    , mAction(NoAction)
{
    for (int i = 0; i < CornerAnchorCount; ++i)
        mRotateHandles[i] = new RotateHandle(static_cast<AnchorPosition>(i));
    for (int i = 0; i < AnchorCount; ++i)
        mResizeHandles[i] = new ResizeHandle(static_cast<AnchorPosition>(i));
}

ObjectSelectionTool::~ObjectSelectionTool()
{
    delete mSelectionRectangle;
    delete mOriginIndicator;

    for (int i = 0; i < CornerAnchorCount; ++i)
        delete mRotateHandles[i];
    for (int i = 0; i < AnchorCount; ++i)
        delete mResizeHandles[i];
}

void ObjectSelectionTool::activate(MapScene *scene)
{
    AbstractObjectTool::activate(scene);

    updateHandles();

    connect(mapDocument(), SIGNAL(objectsChanged(QList<MapObject*>)),
            this, SLOT(updateHandles()));
    connect(mapDocument(), SIGNAL(mapChanged()),
            this, SLOT(updateHandles()));
    connect(scene, SIGNAL(selectedObjectItemsChanged()),
            this, SLOT(updateHandles()));

    connect(mapDocument(), SIGNAL(objectsRemoved(QList<MapObject*>)),
            this, SLOT(objectsRemoved(QList<MapObject*>)));

    scene->addItem(mOriginIndicator);
    for (int i = 0; i < CornerAnchorCount; ++i)
        scene->addItem(mRotateHandles[i]);
    for (int i = 0; i < AnchorCount; ++i)
        scene->addItem(mResizeHandles[i]);
}

void ObjectSelectionTool::deactivate(MapScene *scene)
{
    scene->removeItem(mOriginIndicator);
    for (int i = 0; i < CornerAnchorCount; ++i)
        scene->removeItem(mRotateHandles[i]);
    for (int i = 0; i < AnchorCount; ++i)
        scene->removeItem(mResizeHandles[i]);

    disconnect(mapDocument(), SIGNAL(objectsChanged(QList<MapObject*>)),
               this, SLOT(updateHandles()));
    disconnect(mapDocument(), SIGNAL(mapChanged()),
               this, SLOT(updateHandles()));
    disconnect(scene, SIGNAL(selectedObjectItemsChanged()),
               this, SLOT(updateHandles()));

    AbstractObjectTool::deactivate(scene);
}

void ObjectSelectionTool::keyPressed(QKeyEvent *event)
{
    if (mAction != NoAction) {
        event->ignore();
        return;
    }

    QPointF moveBy;

    switch (event->key()) {
    case Qt::Key_Up:    moveBy = QPointF(0, -1); break;
    case Qt::Key_Down:  moveBy = QPointF(0, 1); break;
    case Qt::Key_Left:  moveBy = QPointF(-1, 0); break;
    case Qt::Key_Right: moveBy = QPointF(1, 0); break;
    default:
        AbstractObjectTool::keyPressed(event);
        return;
    }

    const QSet<MapObjectItem*> &items = mapScene()->selectedObjectItems();
    const Qt::KeyboardModifiers modifiers = event->modifiers();

    if (moveBy.isNull() || items.isEmpty() || (modifiers & Qt::ControlModifier)) {
        event->ignore();
        return;
    }

    const bool moveFast = modifiers & Qt::ShiftModifier;
    const bool snapToFineGrid = Preferences::instance()->snapToFineGrid();

    if (moveFast) {
        // TODO: This only makes sense for orthogonal maps
        moveBy.rx() *= mapDocument()->map()->tileWidth();
        moveBy.ry() *= mapDocument()->map()->tileHeight();
        if (snapToFineGrid)
            moveBy /= Preferences::instance()->gridFine();
    }

    QUndoStack *undoStack = mapDocument()->undoStack();
    undoStack->beginMacro(tr("Move %n Object(s)", "", items.size()));
    int i = 0;
    foreach (MapObjectItem *objectItem, items) {
        MapObject *object = objectItem->mapObject();
        const QPointF oldPos = object->position();
        object->setPosition(oldPos + moveBy);
        undoStack->push(new MoveMapObject(mapDocument(), object, oldPos));
        ++i;
    }
    undoStack->endMacro();
}

void ObjectSelectionTool::mouseEntered()
{
}

void ObjectSelectionTool::mouseMoved(const QPointF &pos,
                                     Qt::KeyboardModifiers modifiers)
{
    AbstractObjectTool::mouseMoved(pos, modifiers);

    if (mAction == NoAction && mMousePressed) {
        QPoint screenPos = QCursor::pos();
        const int dragDistance = (mScreenStart - screenPos).manhattanLength();
        if (dragDistance >= QApplication::startDragDistance()) {
            // Holding shift makes sure we'll start a selection operation
            if ((mClickedObjectItem || modifiers & Qt::AltModifier) &&
                    !(modifiers & Qt::ShiftModifier)) {
                startMoving(modifiers);
            } else if (mClickedRotateHandle) {
                startRotating();
            } else if (mClickedResizeHandle) {
                startResizing();
            } else {
                startSelecting();
            }
        }
    }

    switch (mAction) {
    case Selecting:
        mSelectionRectangle->setRectangle(QRectF(mStart, pos).normalized());
        break;
    case Moving:
        updateMovingItems(pos, modifiers);
        break;
    case Rotating:
        updateRotatingItems(pos, modifiers);
        break;
    case Resizing:
        updateResizingItems(pos, modifiers);
        break;
    case NoAction:
        break;
    }
}

static QGraphicsView *findView(QGraphicsSceneEvent *event)
{
    if (QWidget *viewport = event->widget())
        return qobject_cast<QGraphicsView*>(viewport->parent());
    return 0;
}

void ObjectSelectionTool::mousePressed(QGraphicsSceneMouseEvent *event)
{
    if (mAction != NoAction) // Ignore additional presses during select/move
        return;

    switch (event->button()) {
    case Qt::LeftButton: {
        mMousePressed = true;
        mStart = event->scenePos();
        mScreenStart = event->screenPos();

        RotateHandle *clickedRotateHandle = 0;
        ResizeHandle *clickedResizeHandle = 0;

        if (QGraphicsView *view = findView(event)) {
            QGraphicsItem *clickedItem = mapScene()->itemAt(event->scenePos(),
                                                            view->transform());

            clickedRotateHandle = dynamic_cast<RotateHandle*>(clickedItem);
            clickedResizeHandle = dynamic_cast<ResizeHandle*>(clickedItem);
        }

        mClickedRotateHandle = clickedRotateHandle;
        mClickedResizeHandle = clickedResizeHandle;
        if (!clickedRotateHandle && !clickedResizeHandle)
            mClickedObjectItem = topMostObjectItemAt(mStart);

        break;
    }
    default:
        AbstractObjectTool::mousePressed(event);
        break;
    }
}

void ObjectSelectionTool::mouseReleased(QGraphicsSceneMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;

    switch (mAction) {
    case NoAction: {
        if (mClickedRotateHandle || mClickedResizeHandle) {
            // Don't change selection as a result of clicking on a handle
            break;
        }
        const Qt::KeyboardModifiers modifiers = event->modifiers();
        if (mClickedObjectItem) {
            QSet<MapObjectItem*> selection = mapScene()->selectedObjectItems();
            if (modifiers & (Qt::ShiftModifier | Qt::ControlModifier)) {
                if (selection.contains(mClickedObjectItem))
                    selection.remove(mClickedObjectItem);
                else
                    selection.insert(mClickedObjectItem);
            } else if (selection.contains(mClickedObjectItem)) {
                // Clicking one of the selected items changes the edit mode
                setMode((mMode == Resize) ? Rotate : Resize);
            } else {
                selection.clear();
                selection.insert(mClickedObjectItem);
                setMode(Resize);
            }
            mapScene()->setSelectedObjectItems(selection);
        } else if (!(modifiers & Qt::ShiftModifier)) {
            mapScene()->setSelectedObjectItems(QSet<MapObjectItem*>());
        }
        break;
    }
    case Selecting:
        updateSelection(event->scenePos(), event->modifiers());
        mapScene()->removeItem(mSelectionRectangle);
        mAction = NoAction;
        break;
    case Moving:
        finishMoving(event->scenePos());
        break;
    case Rotating:
        finishRotating(event->scenePos());
        break;
    case Resizing:
        finishResizing(event->scenePos());
        break;
    }

    mMousePressed = false;
    mClickedObjectItem = 0;
    mClickedRotateHandle = 0;
    mClickedResizeHandle = 0;
}

void ObjectSelectionTool::modifiersChanged(Qt::KeyboardModifiers modifiers)
{
    mModifiers = modifiers;
}

void ObjectSelectionTool::languageChanged()
{
    setName(tr("Select Objects"));
    setShortcut(QKeySequence(tr("S")));
}

static QPointF alignmentOffset(QRectF &r, Alignment alignment)
{
    switch (alignment) {
    case TopLeft:       break;
    case Top:           return QPointF(r.width() / 2, 0);               break;
    case TopRight:      return QPointF(r.width(), 0);                   break;
    case Left:          return QPointF(0, r.height() / 2);              break;
    case Center:        return QPointF(r.width() / 2, r.height() / 2);  break;
    case Right:         return QPointF(r.width(), r.height() / 2);      break;
    case BottomLeft:    return QPointF(0, r.height());                  break;
    case Bottom:        return QPointF(r.width() / 2, r.height());      break;
    case BottomRight:   return QPointF(r.width(), r.height());          break;
    }
    return QPointF();
}

// TODO: Check whether this function should be moved into MapObject::bounds
static void align(QRectF &r, Alignment alignment)
{
    r.translate(-alignmentOffset(r, alignment));
}

static void unalign(QRectF &r, Alignment alignment)
{
    r.translate(alignmentOffset(r, alignment));
}

static QRectF pixelBounds(const MapObject *object)
{
    Q_ASSERT(object->cell().isEmpty()); // tile objects only have screen bounds

    switch (object->shape()) {
    case MapObject::Ellipse:
    case MapObject::Rectangle: {
        QRectF bounds(object->bounds());
        align(bounds, object->alignment());
        return bounds;
    }
    case MapObject::Polygon:
    case MapObject::Polyline: {
        // Alignment is irrelevant for polygon objects since they have no size
        const QPointF &pos = object->position();
        const QPolygonF polygon = object->polygon().translated(pos);
        return polygon.boundingRect();
    }
    }

    return QRectF();
}

static bool resizeInPixelSpace(const MapObject *object)
{
    return object->cell().isEmpty();
}

/* This function returns the actual bounds of the object, as opposed to the
 * bounds of its visualization that the MapRenderer::boundingRect function
 * returns.
 *
 * Before calculating the final bounding rectangle, the object is transformed
 * by the given transformation.
 */
static QRectF objectBounds(const MapObject *object,
                           const MapRenderer *renderer,
                           const QTransform &transform)
{
    if (!object->cell().isEmpty()) {
        // Tile objects can have a tile offset, which is scaled along with the image
        const Tile *tile = object->cell().tile;
        const QSize imgSize = tile->image().size();
        const QPointF position = renderer->pixelToScreenCoords(object->position());

        const QPoint tileOffset = tile->tileset()->tileOffset();
        const QSizeF objectSize = object->size();
        const qreal scaleX = imgSize.width() > 0 ? objectSize.width() / imgSize.width() : 0;
        const qreal scaleY = imgSize.height() > 0 ? objectSize.height() / imgSize.height() : 0;

        QRectF bounds(position.x() + (tileOffset.x() * scaleX),
                      position.y() + (tileOffset.y() * scaleY),
                      objectSize.width(),
                      objectSize.height());

        align(bounds, object->alignment());

        return transform.mapRect(bounds);
    } else {
        switch (object->shape()) {
        case MapObject::Ellipse:
        case MapObject::Rectangle: {
            QRectF bounds(object->bounds());
            align(bounds, object->alignment());
            QPolygonF screenPolygon = renderer->pixelToScreenCoords(bounds);
            return transform.map(screenPolygon).boundingRect();
        }
        case MapObject::Polygon:
        case MapObject::Polyline: {
            // Alignment is irrelevant for polygon objects since they have no size
            const QPointF &pos = object->position();
            const QPolygonF polygon = object->polygon().translated(pos);
            QPolygonF screenPolygon = renderer->pixelToScreenCoords(polygon);
            return transform.map(screenPolygon).boundingRect();
        }
        }
    }

    return QRectF();
}

static QTransform rotateAt(const QPointF &position, qreal rotation)
{
    QTransform transform;
    transform.translate(position.x(), position.y());
    transform.rotate(rotation);
    transform.translate(-position.x(), -position.y());
    return transform;
}

static QTransform objectTransform(MapObject *object, MapRenderer *renderer)
{
    if (object->rotation() != 0) {
        const QPointF pos = renderer->pixelToScreenCoords(object->position());
        return rotateAt(pos, object->rotation());
    }
    return QTransform();
}

void ObjectSelectionTool::updateHandles()
{
    if (mAction == Moving || mAction == Rotating || mAction == Resizing)
        return;

    const QList<MapObject*> &objects = mapDocument()->selectedObjects();
    const bool showHandles = objects.size() > 0;

    if (showHandles) {
        MapRenderer *renderer = mapDocument()->renderer();
        QRectF boundingRect = objectBounds(objects.first(), renderer,
                                            objectTransform(objects.first(), renderer));

        for (int i = 1; i < objects.size(); ++i) {
            MapObject *object = objects.at(i);
            boundingRect |= objectBounds(object, renderer,
                                         objectTransform(object, renderer));
        }

        QPointF topLeft = boundingRect.topLeft();
        QPointF topRight = boundingRect.topRight();
        QPointF bottomLeft = boundingRect.bottomLeft();
        QPointF bottomRight = boundingRect.bottomRight();
        QPointF center = boundingRect.center();

        qreal handleRotation = 0;

        // If there is only one object selected, align to its orientation.
        if (objects.size() == 1) {
            MapObject *object = objects.first();

            handleRotation = object->rotation();

            if (resizeInPixelSpace(object)) {
                QRectF bounds = pixelBounds(object);

                QTransform transform(objectTransform(object, renderer));
                topLeft = transform.map(renderer->pixelToScreenCoords(bounds.topLeft()));
                topRight = transform.map(renderer->pixelToScreenCoords(bounds.topRight()));
                bottomLeft = transform.map(renderer->pixelToScreenCoords(bounds.bottomLeft()));
                bottomRight = transform.map(renderer->pixelToScreenCoords(bounds.bottomRight()));
                center = transform.map(renderer->pixelToScreenCoords(bounds.center()));

                // Ugly hack to make handles appear nicer in this case
                if (mapDocument()->map()->orientation() == Map::Isometric)
                    handleRotation += 45;
            } else {
                QRectF bounds = objectBounds(object, renderer, QTransform());

                QTransform transform(objectTransform(object, renderer));
                topLeft = transform.map(bounds.topLeft());
                topRight = transform.map(bounds.topRight());
                bottomLeft = transform.map(bounds.bottomLeft());
                bottomRight = transform.map(bounds.bottomRight());
                center = transform.map(bounds.center());
            }
        }

        mOriginIndicator->setPos(center);

        mRotateHandles[TopLeftAnchor]->setPos(topLeft);
        mRotateHandles[TopRightAnchor]->setPos(topRight);
        mRotateHandles[BottomLeftAnchor]->setPos(bottomLeft);
        mRotateHandles[BottomRightAnchor]->setPos(bottomRight);

        QPointF top = (topLeft + topRight) / 2;
        QPointF left = (topLeft + bottomLeft) / 2;
        QPointF right = (topRight + bottomRight) / 2;
        QPointF bottom = (bottomLeft + bottomRight) / 2;
        
        mResizeHandles[TopAnchor]->setPos(top);
        mResizeHandles[TopAnchor]->setResizingOrigin(bottom);
        mResizeHandles[LeftAnchor]->setPos(left);
        mResizeHandles[LeftAnchor]->setResizingOrigin(right);
        mResizeHandles[RightAnchor]->setPos(right);
        mResizeHandles[RightAnchor]->setResizingOrigin(left);
        mResizeHandles[BottomAnchor]->setPos(bottom);
        mResizeHandles[BottomAnchor]->setResizingOrigin(top);
        
        mResizeHandles[TopLeftAnchor]->setPos(topLeft);
        mResizeHandles[TopLeftAnchor]->setResizingOrigin(bottomRight);
        mResizeHandles[TopRightAnchor]->setPos(topRight);
        mResizeHandles[TopRightAnchor]->setResizingOrigin(bottomLeft);
        mResizeHandles[BottomLeftAnchor]->setPos(bottomLeft);
        mResizeHandles[BottomLeftAnchor]->setResizingOrigin(topRight);
        mResizeHandles[BottomRightAnchor]->setPos(bottomRight);
        mResizeHandles[BottomRightAnchor]->setResizingOrigin(topLeft);

        for (int i = 0; i < CornerAnchorCount; ++i)
            mRotateHandles[i]->setRotation(handleRotation);
        for (int i = 0; i < AnchorCount; ++i)
            mResizeHandles[i]->setRotation(handleRotation);
    }

    updateHandleVisibility();
}

void ObjectSelectionTool::updateHandleVisibility()
{
    const bool hasSelection = !mapDocument()->selectedObjects().isEmpty();
    const bool showHandles = hasSelection && (mAction == NoAction || mAction == Selecting);
    const bool showOrigin = hasSelection &&
            mAction != Moving && (mMode == Rotate || mAction == Resizing);

    for (int i = 0; i < CornerAnchorCount; ++i)
        mRotateHandles[i]->setVisible(showHandles && mMode == Rotate);
    for (int i = 0; i < AnchorCount; ++i)
        mResizeHandles[i]->setVisible(showHandles && mMode == Resize);

    mOriginIndicator->setVisible(showOrigin);
}

void ObjectSelectionTool::objectsRemoved(const QList<MapObject *> &objects)
{
    if (mAction != Moving && mAction != Rotating)
        return;

    // Abort move/rotate to avoid crashing...
    // TODO: This should really not be allowed to happen in the first place.
    int i = 0;
    foreach (const MovingObject &object, mMovingObjects) {
        MapObject *mapObject = object.item->mapObject();
        if (!objects.contains(mapObject)) {
            mapObject->setPosition(object.oldPosition);
            object.item->setPos(object.oldItemPosition);
            if (mAction == Rotating)
                object.item->setObjectRotation(object.oldRotation);
        }
        ++i;
    }

    mMovingObjects.clear();
}

void ObjectSelectionTool::updateSelection(const QPointF &pos,
                                          Qt::KeyboardModifiers modifiers)
{
    QRectF rect = QRectF(mStart, pos).normalized();

    // Make sure the rect has some contents, otherwise intersects returns false
    rect.setWidth(qMax(qreal(1), rect.width()));
    rect.setHeight(qMax(qreal(1), rect.height()));

    QSet<MapObjectItem*> selectedItems;

    foreach (QGraphicsItem *item, mapScene()->items(rect)) {
        MapObjectItem *mapObjectItem = dynamic_cast<MapObjectItem*>(item);
        if (mapObjectItem)
            selectedItems.insert(mapObjectItem);
    }

    if (modifiers & (Qt::ControlModifier | Qt::ShiftModifier))
        selectedItems |= mapScene()->selectedObjectItems();
    else
        setMode(Resize);

    mapScene()->setSelectedObjectItems(selectedItems);
}

void ObjectSelectionTool::startSelecting()
{
    mAction = Selecting;
    mapScene()->addItem(mSelectionRectangle);
}

void ObjectSelectionTool::startMoving(Qt::KeyboardModifiers modifiers)
{
    // Move only the clicked item, if it was not part of the selection
    if (mClickedObjectItem && !(modifiers & Qt::AltModifier)) {
        if (!mapScene()->selectedObjectItems().contains(mClickedObjectItem))
            mapScene()->setSelectedObjectItems(QSet<MapObjectItem*>() << mClickedObjectItem);
    }

    saveSelectionState();

    mAction = Moving;
    mAlignPosition = mMovingObjects.first().oldPosition;

    foreach (const MovingObject &object, mMovingObjects) {
        const QPointF &pos = object.oldPosition;
        if (pos.x() < mAlignPosition.x())
            mAlignPosition.setX(pos.x());
        if (pos.y() < mAlignPosition.y())
            mAlignPosition.setY(pos.y());
    }

    updateHandleVisibility();
}

void ObjectSelectionTool::updateMovingItems(const QPointF &pos,
                                            Qt::KeyboardModifiers modifiers)
{
    MapRenderer *renderer = mapDocument()->renderer();
    QPointF diff = pos - mStart;

    diff = snapToGrid(diff, modifiers);

    foreach (const MovingObject &object, mMovingObjects) {
        const QPointF newPixelPos = object.oldItemPosition + diff;
        const QPointF newPos = renderer->screenToPixelCoords(newPixelPos);
        object.item->setPos(newPixelPos);
        object.item->mapObject()->setPosition(newPos);

        ObjectGroup *objectGroup = object.item->mapObject()->objectGroup();
        if (objectGroup->drawOrder() == ObjectGroup::TopDownOrder)
            object.item->setZValue(newPixelPos.y());
    }
}

void ObjectSelectionTool::finishMoving(const QPointF &pos)
{
    Q_ASSERT(mAction == Moving);
    mAction = NoAction;
    updateHandles();

    if (mStart == pos) // Move is a no-op
        return;

    QUndoStack *undoStack = mapDocument()->undoStack();
    undoStack->beginMacro(tr("Move %n Object(s)", "", mMovingObjects.size()));
    foreach (const MovingObject &object, mMovingObjects) {
        undoStack->push(new MoveMapObject(mapDocument(),
                                          object.item->mapObject(),
                                          object.oldPosition));
    }
    undoStack->endMacro();

    mMovingObjects.clear();
}

void ObjectSelectionTool::startRotating()
{
    mAction = Rotating;
    mOrigin = mOriginIndicator->pos();

    saveSelectionState();
    updateHandleVisibility();
}

void ObjectSelectionTool::updateRotatingItems(const QPointF &pos,
                                              Qt::KeyboardModifiers modifiers)
{
    MapRenderer *renderer = mapDocument()->renderer();

    const QPointF startDiff = mOrigin - mStart;
    const QPointF currentDiff = mOrigin - pos;

    const qreal startAngle = std::atan2(startDiff.y(), startDiff.x());
    const qreal currentAngle = std::atan2(currentDiff.y(), currentDiff.x());
    qreal angleDiff = currentAngle - startAngle;

    const qreal snap = 15 * M_PI / 180; // 15 degrees in radians
    if (modifiers & Qt::ControlModifier)
        angleDiff = std::floor((angleDiff + snap / 2) / snap) * snap;

    foreach (const MovingObject &object, mMovingObjects) {
        const QPointF oldRelPos = object.oldItemPosition - mOrigin;
        const qreal sn = std::sin(angleDiff);
        const qreal cs = std::cos(angleDiff);
        const QPointF newRelPos(oldRelPos.x() * cs - oldRelPos.y() * sn,
                                oldRelPos.x() * sn + oldRelPos.y() * cs);
        const QPointF newPixelPos = mOrigin + newRelPos;
        const QPointF newPos = renderer->screenToPixelCoords(newPixelPos);

        const qreal newRotation = object.oldRotation + angleDiff * 180 / M_PI;

        object.item->setPos(newPixelPos);
        object.item->mapObject()->setPosition(newPos);
        object.item->setObjectRotation(newRotation);
    }
}

void ObjectSelectionTool::finishRotating(const QPointF &pos)
{
    Q_ASSERT(mAction == Rotating);
    mAction = NoAction;
    updateHandles();

    if (mStart == pos) // No rotation at all
        return;

    QUndoStack *undoStack = mapDocument()->undoStack();
    undoStack->beginMacro(tr("Rotate %n Object(s)", "", mMovingObjects.size()));
    foreach (const MovingObject &object, mMovingObjects) {
        MapObject *mapObject = object.item->mapObject();
        undoStack->push(new MoveMapObject(mapDocument(), mapObject, object.oldPosition));
        undoStack->push(new RotateMapObject(mapDocument(), mapObject, object.oldRotation));
    }
    undoStack->endMacro();

    mMovingObjects.clear();
}


void ObjectSelectionTool::startResizing()
{
    mAction = Resizing;
    mOrigin = mOriginIndicator->pos();

    mResizingLimitHorizontal = mClickedResizeHandle->resizingLimitHorizontal();
    mResizingLimitVertical = mClickedResizeHandle->resizingLimitVertical();

    mStart = mClickedResizeHandle->pos();

    saveSelectionState();
    updateHandleVisibility();
}

void ObjectSelectionTool::updateResizingItems(const QPointF &pos,
                                              Qt::KeyboardModifiers modifiers)
{
    MapRenderer *renderer = mapDocument()->renderer();

    QPointF resizingOrigin = mClickedResizeHandle->resizingOrigin();
    if (modifiers & Qt::ShiftModifier)
        resizingOrigin = mOrigin;

    mOriginIndicator->setPos(resizingOrigin);

    /* Alternative toggle snap modifier, since Control is taken by the preserve
     * aspect ratio option.
     */
    SnapHelper snapHelper(renderer);
    if (modifiers & Qt::AltModifier)
        snapHelper.toggleSnap();
    QPointF pixelPos = renderer->screenToPixelCoords(pos);
    snapHelper.snap(pixelPos);
    QPointF snappedScreenPos = renderer->pixelToScreenCoords(pixelPos);

    QPointF diff = snappedScreenPos - resizingOrigin;
    QPointF startDiff = mStart - resizingOrigin;

    if (mMovingObjects.size() == 1) {
        /* For single items the resizing is performed in object space in order
         * to handle different scaling on X and Y axis as well as to improve
         * handling of 0-sized objects.
         */
        updateResizingSingleItem(resizingOrigin, snappedScreenPos, modifiers);
        return;
    }

    /* Calculate the scaling factor. Minimum is 1% to protect against making
     * everything 0-sized and non-recoverable (it's still possibly to run into
     * problems by repeatedly scaling down to 1%, but that's asking for it)
     */
    qreal scale;
    if (mResizingLimitHorizontal) {
        scale = qMax((qreal)0.01, diff.y() / startDiff.y());
    } else if (mResizingLimitVertical) {
        scale = qMax((qreal)0.01, diff.x() / startDiff.x());
    } else {
        scale = qMin(qMax((qreal)0.01, diff.x() / startDiff.x()),
                     qMax((qreal)0.01, diff.y() / startDiff.y()));
    }

    foreach (const MovingObject &object, mMovingObjects) {
        const QPointF oldRelPos = object.oldItemPosition - resizingOrigin;
        const QPointF scaledRelPos(oldRelPos.x() * scale,
                                   oldRelPos.y() * scale);
        const QPointF newScreenPos = resizingOrigin + scaledRelPos;
        const QPointF newPos = renderer->screenToPixelCoords(newScreenPos);
        const QSizeF origSize = object.oldSize;
        const QSizeF newSize(origSize.width() * scale,
                             origSize.height() * scale);

        if (object.item->mapObject()->polygon().isEmpty() == false) {
            // For polygons, we have to scale in object space.
            qreal rotation = object.item->rotation() * M_PI / -180;
            const qreal sn = std::sin(rotation);
            const qreal cs = std::cos(rotation);
            
            const QPolygonF &oldPolygon = object.oldPolygon;
            QPolygonF newPolygon(oldPolygon.size());
            for (int n = 0; n < oldPolygon.size(); ++n) {
                const QPointF oldPoint(oldPolygon[n]);
                const QPointF rotPoint(oldPoint.x() * cs + oldPoint.y() * sn,
                                       oldPoint.y() * cs - oldPoint.x() * sn);
                const QPointF scaledPoint(rotPoint.x() * scale, rotPoint.y() * scale);
                const QPointF newPoint(scaledPoint.x() * cs - scaledPoint.y() * sn,
                                       scaledPoint.y() * cs + scaledPoint.x() * sn);
                newPolygon[n] = newPoint;
            }
            object.item->mapObject()->setPolygon(newPolygon);
        }
        
        object.item->resizeObject(newSize);
        object.item->setPos(newScreenPos);
        object.item->mapObject()->setPosition(newPos);
    }
}

void ObjectSelectionTool::updateResizingSingleItem(const QPointF &resizingOrigin,
                                                   const QPointF &screenPos,
                                                   Qt::KeyboardModifiers modifiers)
{
    const MapRenderer *renderer = mapDocument()->renderer();
    const MovingObject &object = mMovingObjects.first();
    MapObject *mapObject = object.item->mapObject();

    /* These transformations undo and redo the object rotation, which is always
     * applied in screen space.
     */
    QTransform unrotate = rotateAt(object.oldItemPosition, -object.oldRotation);
    QTransform rotate = rotateAt(object.oldItemPosition, object.oldRotation);

    QPointF origin = resizingOrigin * unrotate;
    QPointF pos = screenPos * unrotate;
    QPointF start = mStart * unrotate;
    QPointF oldPos = object.oldItemPosition;

    /* In order for the resizing to work somewhat sanely in isometric mode,
     * the resizing is performed in pixel space except for tile objects, which
     * are not affected by isometric projection apart from their position.
     */
    const bool pixelSpace = resizeInPixelSpace(mapObject);
    const bool preserveAspect = modifiers & Qt::ControlModifier;

    if (pixelSpace) {
        origin = renderer->screenToPixelCoords(origin);
        pos = renderer->screenToPixelCoords(pos);
        start = renderer->screenToPixelCoords(start);
        oldPos = object.oldPosition;
    }

    QPointF newPos = oldPos;
    QSizeF newSize = object.oldSize;

    /* In case one of the anchors was used as-is, the desired size can be
     * derived directly from the distance from the origin for rectangle
     * and ellipse objects. This allows scaling up a 0-sized object without
     * dealing with infinite scaling factor issues.
     *
     * For obvious reasons this can't work on polygons or polylines, nor when
     * preserving the aspect ratio.
     */
    if (mClickedResizeHandle->resizingOrigin() == resizingOrigin &&
            (mapObject->shape() == MapObject::Rectangle ||
             mapObject->shape() == MapObject::Ellipse) && !preserveAspect) {

        QRectF newBounds = QRectF(newPos, newSize);
        align(newBounds, mapObject->alignment());

        switch (mClickedResizeHandle->anchorPosition()) {
        case LeftAnchor:
        case TopLeftAnchor:
        case BottomLeftAnchor:
            newBounds.setLeft(qMin(pos.x(), origin.x())); break;
        case RightAnchor:
        case TopRightAnchor:
        case BottomRightAnchor:
            newBounds.setRight(qMax(pos.x(), origin.x())); break;
        default:
            // nothing to do on this axis
            break;
        }

        switch (mClickedResizeHandle->anchorPosition()) {
        case TopAnchor:
        case TopLeftAnchor:
        case TopRightAnchor:
            newBounds.setTop(qMin(pos.y(), origin.y())); break;
        case BottomAnchor:
        case BottomLeftAnchor:
        case BottomRightAnchor:
            newBounds.setBottom(qMax(pos.y(), origin.y())); break;
        default:
            // nothing to do on this axis
            break;
        }

        unalign(newBounds, mapObject->alignment());

        newSize = newBounds.size();
        newPos = newBounds.topLeft();
    } else {
        const QPointF relPos = pos - origin;
        const QPointF startDiff = start - origin;

        QSizeF scalingFactor(qMax((qreal)0.01, relPos.x() / startDiff.x()),
                             qMax((qreal)0.01, relPos.y() / startDiff.y()));

        if (mResizingLimitHorizontal) {
            scalingFactor.setWidth(preserveAspect ? scalingFactor.height() : 1);
        } else if (mResizingLimitVertical) {
            scalingFactor.setHeight(preserveAspect ? scalingFactor.width() : 1);
        } else if (preserveAspect) {
            qreal scale = qMin(scalingFactor.width(), scalingFactor.height());
            scalingFactor.setWidth(scale);
            scalingFactor.setHeight(scale);
        }

        QPointF oldRelPos = oldPos - origin;
        newPos = origin + QPointF(oldRelPos.x() * scalingFactor.width(),
                                  oldRelPos.y() * scalingFactor.height());

        newSize.rwidth() *= scalingFactor.width();
        newSize.rheight() *= scalingFactor.height();

        if (!object.oldPolygon.isEmpty()) {
            QPolygonF newPolygon(object.oldPolygon.size());
            for (int n = 0; n < object.oldPolygon.size(); ++n) {
                const QPointF &point = object.oldPolygon[n];
                newPolygon[n] = QPointF(point.x() * scalingFactor.width(),
                                        point.y() * scalingFactor.height());
            }
            mapObject->setPolygon(newPolygon);
        }
    }

    if (pixelSpace)
        newPos = renderer->pixelToScreenCoords(newPos);

    newPos = renderer->screenToPixelCoords(newPos * rotate);

    object.item->resizeObject(newSize);
    object.item->setPos(renderer->pixelToScreenCoords(newPos));
    mapObject->setPosition(newPos);
}

void ObjectSelectionTool::finishResizing(const QPointF &pos)
{
    Q_ASSERT(mAction == Resizing);
    mAction = NoAction;
    updateHandles();

    if (mStart == pos) // No scaling at all
        return;

    QUndoStack *undoStack = mapDocument()->undoStack();
    undoStack->beginMacro(tr("Resize %n Object(s)", "", mMovingObjects.size()));
    foreach (const MovingObject &object, mMovingObjects) {
        MapObject *mapObject = object.item->mapObject();
        undoStack->push(new MoveMapObject(mapDocument(), mapObject, object.oldPosition));
        undoStack->push(new ResizeMapObject(mapDocument(), mapObject, object.oldSize));
        
        if (!object.oldPolygon.isEmpty())
            undoStack->push(new ChangePolygon(mapDocument(), mapObject, object.oldPolygon));
    }
    undoStack->endMacro();

    mMovingObjects.clear();
}

void ObjectSelectionTool::setMode(Mode mode)
{
    if (mMode != mode) {
        mMode = mode;
        updateHandles();
    }
}

void ObjectSelectionTool::saveSelectionState()
{
    mMovingObjects.clear();

    // Remember the initial state before moving, resizing or rotating
    foreach (MapObjectItem *item, mapScene()->selectedObjectItems()) {
        MapObject *mapObject = item->mapObject();
        MovingObject object = {
            item,
            item->pos(),
            mapObject->position(),
            mapObject->size(),
            mapObject->polygon(),
            mapObject->rotation()
        };
        mMovingObjects.append(object);
    }
}

const QPointF ObjectSelectionTool::snapToGrid(const QPointF &diff,
                                              Qt::KeyboardModifiers modifiers)
{
    MapRenderer *renderer = mapDocument()->renderer();
    SnapHelper snapHelper(renderer, modifiers);

    if (snapHelper.snaps()) {
        const QPointF alignScreenPos = renderer->pixelToScreenCoords(mAlignPosition);
        const QPointF newAlignScreenPos = alignScreenPos + diff;

        QPointF newAlignPixelPos = renderer->screenToPixelCoords(newAlignScreenPos);
        snapHelper.snap(newAlignPixelPos);

        return renderer->pixelToScreenCoords(newAlignPixelPos) - alignScreenPos;
    }

    return diff;
}
