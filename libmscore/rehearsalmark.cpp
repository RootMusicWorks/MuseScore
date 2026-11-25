//=============================================================================
//  MuseScore
//  Music Composition & Notation
//
//  Copyright (C) 2011 Werner Schweer
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2
//  as published by the Free Software Foundation and appearing in
//  the file LICENCE.GPL
//=============================================================================

#include "box.h"
#include "mscore.h"
#include "score.h"
#include "rehearsalmark.h"
#include "measure.h"
#include "staff.h"
#include "stafflines.h"
#include "system.h"
#include "xml.h"

namespace Ms {

//---------------------------------------------------------
//   rehearsalMarkStyle
//---------------------------------------------------------

static const ElementStyle rehearsalMarkStyle {
      { Sid::rehearsalMarkPlacement, Pid::PLACEMENT },
      { Sid::rehearsalMarkMinDistance, Pid::MIN_DISTANCE },
      };

//---------------------------------------------------------
//   RehearsalMark
//---------------------------------------------------------

RehearsalMark::RehearsalMark(Score* s)
   : TextBase(s, Tid::REHEARSAL_MARK)
      {
      initElementStyle(&rehearsalMarkStyle);
      setSystemFlag(true);
      }

void RehearsalMark::setNewInsertionDefaults()
      {
      setAutoplace(false);
      setSizeIsSpatiumDependent(false);
      setPropertyFlags(Pid::SIZE_SPATIUM_DEPENDENT, PropertyFlags::UNSTYLED);
      }

QRectF RehearsalMark::frameBounds() const
      {
      if (!hasFrame())
            return bbox();
      const qreal frameSpatium = sizeIsSpatiumDependent() ? spatium() : MScore::baseStyle().value(Sid::spatium).toDouble();
      const qreal halfStroke = frameWidth().val() * frameSpatium * 0.5;
      return frame.adjusted(-halfStroke, -halfStroke, halfStroke, halfStroke);
      }

Spatium RehearsalMark::setupAutomaticFrame(Measure* selectedMeasure, int firstStaff, int endStaff)
      {
      _framePlacement = FramePlacement::Automatic;
      setOffset(QPointF());
      setPropertyFlags(Pid::OFFSET, PropertyFlags::UNSTYLED);

      for (int i = firstStaff; i < endStaff; ++i) {
            Staff* staff = score()->staff(i);
            if (!staff || !staff->isTabStaff(selectedMeasure->tick()) || staff->invisible(selectedMeasure->tick())
                || !selectedMeasure->visible(i))
                  continue;
            if (score()->isMaster())
                  _frameTabStaff = i;
            else {
                  for (const Staff* linked : staff->staffList()) {
                        if (linked->score() == score()->masterScore()) {
                              _frameTabStaff = linked->idx();
                              break;
                              }
                        }
                  }
            break;
            }

      TextBase::layout();
      return Spatium(frameBounds().width() / score()->spatium() + 2.0);
      }

bool RehearsalMark::readProperties(XmlReader& xml)
      {
      if (xml.name() == "frameTabStaff") {
            _frameTabStaff = xml.readInt() - 1;
            return true;
            }
      if (xml.name() != "placeInPrecedingFrame")
            return TextBase::readProperties(xml);
      // Existing scores store this as a boolean. Keep their extra 1sp shift.
      const QString value = xml.readElementText();
      if (value == QStringLiteral("3")) {
            _framePlacement = FramePlacement::Automatic;
            if (isStyled(Pid::OFFSET)) {
                  setOffset(QPointF());
                  setPropertyFlags(Pid::OFFSET, PropertyFlags::UNSTYLED);
                  }
            }
      else if (value == QStringLiteral("2"))
            _framePlacement = FramePlacement::Offset;
      else
            _framePlacement = value.isEmpty() || value.toInt() != 0 ? FramePlacement::Legacy : FramePlacement::None;
      return true;
      }

void RehearsalMark::write(XmlWriter& xml) const
      {
      if (!xml.canWrite(this))
            return;
      xml.stag(this);
      writeProperties(xml);
      if (_framePlacement != FramePlacement::None)
            xml.tag("placeInPrecedingFrame", int(_framePlacement));
      if (_framePlacement == FramePlacement::Automatic && _frameTabStaff >= 0)
            xml.tag("frameTabStaff", _frameTabStaff + 1);
      xml.etag();
      }

//---------------------------------------------------------
//   layout
//---------------------------------------------------------

void RehearsalMark::layout()
      {
      TextBase::layout();
      if (_framePlacement == FramePlacement::Automatic)
            rypos() = styleValue(Pid::OFFSET, getPropertyStyle(Pid::OFFSET)).toPointF().y();

      Segment* s = segment();
      if (s) {
            if (s->rtick().isZero()) {
                  // first CR of measure, alignment is hcenter or right (the usual cases)
                  // align with barline, point just after header, or start of measure depending on context

                  Measure* m = s->measure();
                  Segment* header = s->prev();  // possibly just a start repeat
                  qreal measureX = -s->x();
                  Segment* repeat = m->findSegmentR(SegmentType::StartRepeatBarLine, Fraction(0, 1));
                  qreal barlineX = repeat ? repeat->x() - s->x() : measureX;
                  System* sys = m->system();
                  bool systemFirst = (sys && m->isFirstInSystem());

                  MeasureBase* previous = m->prev();
                  if (_framePlacement != FramePlacement::None && score()->showVBox() && sys && previous
                      && previous->isHBox() && previous->system() == sys) {
                        const HBox* frame = toHBox(previous);
                        const qreal frameCenter = frame->x() + frame->point(frame->boxWidth()) * 0.5;
                        const QRectF bounds = _framePlacement == FramePlacement::Automatic ? frameBounds() : bbox();
                        rxpos() = frameCenter - m->x() - s->x() - bounds.center().x();
                        if (_framePlacement == FramePlacement::Legacy)
                              rypos() += spatium();
                        }
                  else if (!header || repeat || !systemFirst) {
                        // no header, or header with repeat, or header mid-system - align with barline
                        rxpos() = barlineX;
                        }
                  else {
                        // header at start of system
                        // align to a point just after the header
                        Element* e = header->element(track());      // TODO: firstVisibleStaff
                        qreal w = e ? e->width() : header->width();
                        rxpos() = header->x() + w - s->x();

                        // special case for right aligned rehearsal marks at start of system
                        // left align with start of measure if that is further left
                        if (align() & Align::RIGHT)
                              rxpos() = qMin(rpos().x(), measureX + width());
                        }
                  }
            autoplaceSegmentElement();
            }
      }

void RehearsalMark::layoutFrameOnTabStaff()
      {
      if (_framePlacement != FramePlacement::Automatic)
            return;
      rypos() = styleValue(Pid::OFFSET, getPropertyStyle(Pid::OFFSET)).toPointF().y();
      if (_frameTabStaff < 0 || !score()->showVBox())
            return;

      Segment* s = segment();
      Measure* measure = s->measure();
      MeasureBase* previous = measure->prev();
      if (!previous || !previous->isHBox() || previous->system() != measure->system())
            return;

      Staff* target = score()->masterScore()->staff(_frameTabStaff);
      if (!target)
            return;
      if (!score()->isMaster()) {
            Staff* partStaff = nullptr;
            for (Staff* linked : target->staffList()) {
                  if (linked->score() == score()) {
                        partStaff = linked;
                        break;
                        }
                  }
            target = partStaff;
            }
      if (!target || !target->isTabStaff(s->tick()) || target->invisible(s->tick()) || !measure->visible(target->idx()))
            return;

      StaffLines* lines = measure->staffLines(target->idx());
      const auto& staffLines = lines->getLines();
      if (staffLines.isEmpty())
            return;
      const qreal staffCenter = lines->pagePos().y() + (staffLines.first().y1() + staffLines.last().y1()) * 0.5;
      rypos() = staffCenter - s->pagePos().y() - frameBounds().center().y();
      }

//---------------------------------------------------------
//   propertyDefault
//---------------------------------------------------------

QVariant RehearsalMark::propertyDefault(Pid id) const
      {
      switch (id) {
            case Pid::OFFSET:
                  if (_framePlacement == FramePlacement::Automatic)
                        return QPointF();
                  break;
            case Pid::SUB_STYLE:
                  return int(Tid::REHEARSAL_MARK);
            case Pid::PLACEMENT:
                  return score()->styleV(Sid::rehearsalMarkPlacement);
            default:
                  break;
            }
      return TextBase::propertyDefault(id);
      }

} // namespace Ms

