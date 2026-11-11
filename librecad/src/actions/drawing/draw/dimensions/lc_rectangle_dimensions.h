/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LC_RECTANGLE_DIMENSIONS_H
#define LC_RECTANGLE_DIMENSIONS_H

#include <QList>

class RS_Document;
class RS_Entity;
class QWidget;
struct LC_DocumentModificationBatch;

namespace LC_RectangleDimensions {
bool isRectangle(const RS_Entity& entity);
bool isDriving(const RS_Entity& entity);
bool create(RS_Document& document, RS_Entity& dimension, QWidget* parent,
            LC_DocumentModificationBatch& batch);
bool edit(RS_Document& document, RS_Entity& dimension, QWidget* parent,
          LC_DocumentModificationBatch& batch);

// An empty list means a position constraint or locked layer prevents moving the rectangle.
QList<RS_Entity*> dragEntities(RS_Document& document, RS_Entity& rectangle);
}

#endif
