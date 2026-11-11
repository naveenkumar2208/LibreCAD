/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "lc_rectangle_dimensions.h"

#include <QCoreApplication>
#include <QInputDialog>
#include <QMessageBox>
#include <QUuid>

#include <cmath>
#include <memory>
#include <optional>
#include <string_view>

#include "drw_base.h"
#include "rs_dimaligned.h"
#include "rs_dimlinear.h"
#include "rs_document.h"
#include "rs_layer.h"
#include "rs_layerlist.h"
#include "rs_polyline.h"

namespace {
constexpr char rectangleApp[] = "FREEDRAW_RECTANGLE";
constexpr char dimensionApp[] = "FREEDRAW_DRIVING_DIMENSION";
constexpr double tolerance = RS_TOLERANCE_ANGLE;

std::string_view entityTag(const RS_Entity& entity, std::string_view app) {
    bool matching = false;
    for (const auto& value : entity.getDrwExtData()) {
        if (!value) {
            continue;
        }
        if (value->code() == 1001) {
            matching = value->type() == DRW_Variant::STRING && value->c_str() == app;
        } else if (matching && value->code() == 1000 && value->type() == DRW_Variant::STRING) {
            return value->c_str();
        }
    }
    return {};
}

void setTag(RS_Entity& entity, const char* app, std::string value) {
    auto data = entity.getDrwExtData();
    data.push_back(std::make_shared<DRW_Variant>(1001, std::string(app)));
    data.push_back(std::make_shared<DRW_Variant>(1000, std::move(value)));
    entity.setDrwExtData(std::move(data));
}

bool near(double first, double second) {
    return std::abs(first - second) <= tolerance;
}

bool samePoint(const RS_Vector& first, const RS_Vector& second) {
    return first.valid && second.valid && near(first.x, second.x) &&
           near(first.y, second.y) && near(first.z, second.z);
}

bool isCorner(const RS_Entity& rectangle, const RS_Vector& point) {
    const RS_Vector low = rectangle.getMin();
    const RS_Vector high = rectangle.getMax();
    return point.valid && (near(point.x, low.x) || near(point.x, high.x)) &&
           (near(point.y, low.y) || near(point.y, high.y));
}

struct Dimension {
    RS_Vector first;
    RS_Vector second;
    bool horizontal;

    double span() const {
        return horizontal ? second.x - first.x : second.y - first.y;
    }
};

std::optional<Dimension> dimensionGeometry(const RS_Entity& entity) {
    if (entity.rtti() == RS2::EntityDimLinear) {
        const auto& dim = static_cast<const RS_DimLinear&>(entity);
        const double angle = dim.getAngle();
        if (near(std::sin(angle), 0.) || near(std::cos(angle), 0.)) {
            return Dimension{dim.getExtensionPoint1(), dim.getExtensionPoint2(), near(std::sin(angle), 0.)};
        }
    }
    if (entity.rtti() == RS2::EntityDimAligned) {
        const auto& dim = static_cast<const RS_DimAligned&>(entity);
        const RS_Vector first = dim.getExtensionPoint1();
        const RS_Vector second = dim.getExtensionPoint2();
        if (near(first.x, second.x) || near(first.y, second.y)) {
            return Dimension{first, second, near(first.y, second.y)};
        }
    }
    return std::nullopt;
}

void setGeometry(RS_Entity& entity, const Dimension& geometry, const RS_Vector& definition) {
    if (entity.rtti() == RS2::EntityDimLinear) {
        auto& dim = static_cast<RS_DimLinear&>(entity);
        dim.setExtensionPoint1(geometry.first);
        dim.setExtensionPoint2(geometry.second);
    } else {
        auto& dim = static_cast<RS_DimAligned&>(entity);
        dim.setExtensionPoint1(geometry.first);
        dim.setExtensionPoint2(geometry.second);
    }
    auto& dim = static_cast<RS_Dimension&>(entity);
    dim.setDefinitionPoint(definition);
    dim.update();
}

struct Binding {
    std::string_view first;
    std::string_view second;
};

Binding binding(const RS_Entity& dimension) {
    const auto tag = entityTag(dimension, dimensionApp);
    const auto separator = tag.find('/');
    if (separator == std::string_view::npos) {
        return {};
    }
    return {tag.substr(0, separator), tag.substr(separator + 1)};
}

RS_Entity* rectangleAt(RS_Document& document, const RS_Vector& point, std::string_view id) {
    RS_Entity* match = nullptr;
    for (auto* entity : document) {
        if (!entity->isAlive() || (id.empty() ? !entity->isVisible() : entityTag(*entity, rectangleApp) != id) ||
            !LC_RectangleDimensions::isRectangle(*entity) || !isCorner(*entity, point)) {
            continue;
        }
        if (match) {
            return nullptr;
        }
        match = entity;
    }
    return match;
}

bool attached(const RS_Entity& rectangle, std::string_view id, const RS_Vector& point) {
    return !id.empty() && entityTag(rectangle, rectangleApp) == id && isCorner(rectangle, point);
}

void replace(LC_DocumentModificationBatch& batch, RS_Entity& original, std::unique_ptr<RS_Entity> clone) {
    clone->clearSelectionFlag();
    batch.dontSetActiveLayerAndPen();
    batch += clone.release();
    batch -= &original;
}

QString tr(const char* text) {
    return QCoreApplication::translate("LC_RectangleDimensions", text);
}

bool changeDimension(RS_Document& document, RS_Entity& dimension, QWidget* parent,
                     LC_DocumentModificationBatch& batch, bool creating) {
    auto geometry = dimensionGeometry(dimension);
    const Binding ids = binding(dimension);
    auto* first = geometry ? rectangleAt(document, geometry->first, ids.first) : nullptr;
    auto* second = geometry ? rectangleAt(document, geometry->second, ids.second) : nullptr;
    if (!first || !second || !geometry || near(geometry->span(), 0.)) {
        if (creating) {
            batch += &dimension;
            return true;
        }
        QMessageBox::information(parent, tr("Driving Dimension"), tr("The dimension's rectangle corners could not be found."));
        return false;
    }

    const bool size = first == second;
    const QString title = size ? (geometry->horizontal ? tr("Rectangle Width") : tr("Rectangle Height")) :
        (geometry->horizontal ? tr("Horizontal Rectangle Spacing") : tr("Vertical Rectangle Spacing"));
    auto* activeLayer = document.getLayerList()->getActive();
    const bool needsAnchorId = creating && entityTag(*first, rectangleApp).empty();
    if (second->isLocked() || dimension.isLocked() || (needsAnchorId && first->isLocked()) ||
        (creating && activeLayer && activeLayer->isLocked())) {
        QMessageBox::information(parent, title, tr("Unlock the rectangle and dimension layers before editing."));
        return false;
    }

    bool accepted = false;
    const double oldSpan = geometry->span();
    // Keep either side long enough for the polyline reader to reconstruct the closing segment.
    const double minimum = size ? 0.000100001 : 0.0000001;
    const double magnitude = QInputDialog::getDouble(parent, title, tr("Value in drawing units"),
        std::abs(oldSpan), minimum, 1.0e15, 9, &accepted);
    if (!accepted) {
        return false;
    }
    const double newSpan = std::copysign(magnitude, oldSpan);
    if (!creating && near(oldSpan, newSpan)) {
        return true;
    }

    const RS_Vector offset = geometry->horizontal ? RS_Vector(newSpan - oldSpan, 0.) : RS_Vector(0., newSpan - oldSpan);
    const RS_Vector factor = geometry->horizontal ? RS_Vector(newSpan / oldSpan, 1., 1.) : RS_Vector(1., newSpan / oldSpan, 1.);
    const auto transform = [&](RS_Vector point) {
        if (size) {
            point.scale(geometry->first, factor);
        } else {
            point += offset;
        }
        return point;
    };

    QList<RS_Entity*> companionDimensions;
    for (auto* entity : document) {
        if (entity == &dimension || !entity->isAlive() || !LC_RectangleDimensions::isDriving(*entity)) {
            continue;
        }
        const auto other = dimensionGeometry(*entity);
        const auto otherIds = binding(*entity);
        const bool atFirst = attached(*second, otherIds.first, other->first);
        const bool atSecond = attached(*second, otherIds.second, other->second);
        if (!atFirst && !atSecond) {
            continue;
        }
        if (atFirst && atSecond && (!size || other->horizontal != geometry->horizontal) && !entity->isLocked()) {
            companionDimensions.append(entity);
        } else if ((atFirst && atSecond) || (atFirst && !samePoint(transform(other->first), other->first)) ||
                   (atSecond && !samePoint(transform(other->second), other->second))) {
            QMessageBox::information(parent, title, tr("This change conflicts with an existing driving dimension."));
            return false;
        }
    }

    std::unique_ptr<RS_Entity> resized(second->clone());
    if (size) {
        resized->scale(geometry->first, factor);
    } else {
        resized->move(offset);
    }
    resized->calculateBorders();
    if (!LC_RectangleDimensions::isRectangle(*resized)) {
        QMessageBox::information(parent, title, tr("The requested value cannot be represented at this drawing position."));
        return false;
    }

    if (creating) {
        if (entityTag(*resized, rectangleApp).empty()) {
            setTag(*resized, rectangleApp, QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());
        }
        std::string firstId(entityTag(*first, rectangleApp));
        if (size) {
            firstId = entityTag(*resized, rectangleApp);
        } else if (firstId.empty()) {
            std::unique_ptr<RS_Entity> anchor(first->clone());
            firstId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
            setTag(*anchor, rectangleApp, firstId);
            replace(batch, *first, std::move(anchor));
        }
        setTag(dimension, dimensionApp, firstId + '/' + std::string(entityTag(*resized, rectangleApp)));
    }

    for (auto* entity : companionDimensions) {
        std::unique_ptr<RS_Entity> clone(entity->clone());
        const auto old = *dimensionGeometry(*entity);
        const Dimension next{transform(old.first), transform(old.second), old.horizontal};
        const RS_Vector definition = static_cast<RS_Dimension*>(entity)->getDefinitionPoint();
        setGeometry(*clone, next, definition + (next.second - old.second));
        replace(batch, *entity, std::move(clone));
    }

    const RS_Vector oldSecond = geometry->second;
    geometry->second = transform(oldSecond);
    const RS_Vector definition = static_cast<RS_Dimension&>(dimension).getDefinitionPoint();
    if (creating) {
        dimension.setLayer(activeLayer);
        dimension.setPen(document.getActivePen());
        setGeometry(dimension, *geometry, definition + (geometry->second - oldSecond));
        batch += &dimension;
    } else {
        std::unique_ptr<RS_Entity> clone(dimension.clone());
        setGeometry(*clone, *geometry, definition + (geometry->second - oldSecond));
        replace(batch, dimension, std::move(clone));
    }
    replace(batch, *second, std::move(resized));
    return true;
}
}

namespace LC_RectangleDimensions {
bool isRectangle(const RS_Entity& entity) {
    if (entity.rtti() != RS2::EntityPolyline || !entity.hasValidBorders()) {
        return false;
    }
    const auto& polyline = static_cast<const RS_Polyline&>(entity);
    const RS_Vector low = entity.getMin();
    const RS_Vector high = entity.getMax();
    if (!polyline.isClosed() || polyline.count() != 4 || near(low.x, high.x) || near(low.y, high.y)) {
        return false;
    }
    unsigned sides = 0;
    for (const auto* segment : polyline) {
        if (segment->rtti() != RS2::EntityLine) {
            return false;
        }
        const RS_Vector start = segment->getStartpoint();
        const RS_Vector end = segment->getEndpoint();
        if (!isCorner(entity, start) || !isCorner(entity, end) || !near(start.z, end.z)) {
            return false;
        }
        if (near(start.x, end.x) && !near(start.y, end.y)) {
            sides |= near(start.x, low.x) ? 1U : 2U;
        } else if (near(start.y, end.y) && !near(start.x, end.x)) {
            sides |= near(start.y, low.y) ? 4U : 8U;
        }
    }
    return sides == 15U;
}

bool isDriving(const RS_Entity& entity) {
    const auto ids = binding(entity);
    return !ids.first.empty() && !ids.second.empty() && dimensionGeometry(entity).has_value();
}

bool create(RS_Document& document, RS_Entity& dimension, QWidget* parent, LC_DocumentModificationBatch& batch) {
    return changeDimension(document, dimension, parent, batch, true);
}

bool edit(RS_Document& document, RS_Entity& dimension, QWidget* parent, LC_DocumentModificationBatch& batch) {
    return changeDimension(document, dimension, parent, batch, false);
}

QList<RS_Entity*> dragEntities(RS_Document& document, RS_Entity& rectangle) {
    if (rectangle.isLocked()) {
        return {};
    }
    QList<RS_Entity*> result{&rectangle};
    if (entityTag(rectangle, rectangleApp).empty()) {
        return result;
    }
    for (auto* entity : document) {
        if (!entity->isAlive() || !isDriving(*entity)) {
            continue;
        }
        const auto geometry = *dimensionGeometry(*entity);
        const auto ids = binding(*entity);
        const bool first = attached(rectangle, ids.first, geometry.first);
        const bool second = attached(rectangle, ids.second, geometry.second);
        if (first && second && !entity->isLocked()) {
            result.append(entity);
        } else if (first || second) {
            return {};
        }
    }
    return result;
}
}
