
//=============================================================================
//  MuseScore
//  Music Composition & Notation
//
//  Copyright (C) 2002-2017 Werner Schweer
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2
//  as published by the Free Software Foundation and appearing in
//  the file LICENCE.GPL
//=============================================================================

#include "scoreview.h"
#include "texttools.h"
#include "libmscore/chordrest.h"
#include "libmscore/fret.h"
#include "libmscore/harmony.h"
#include "libmscore/score.h"
#include "libmscore/segment.h"
#include "libmscore/measure.h"
#include "libmscore/rest.h"
#include "libmscore/staff.h"
#include "libmscore/timesig.h"

namespace Ms {

//---------------------------------------------------------
//   harmonyTab
//---------------------------------------------------------

void ScoreView::harmonyTab(bool back)
      {
      Harmony* harmony = toHarmony(editData.element);
      if (!harmony->parent())
            return;
      Segment* segment = harmony->getParentSeg();
      if (!segment)
            return;
      Measure* current = _score->tick2measure(segment->tick());
      Measure* target = back ? current->prevMeasure() : current->nextMeasure();
      if (target)
            harmonyEditAt(target, target->tick());
      }

void ScoreView::harmonyAdvanceByTimeSignature()
      {
      Harmony* harmony = toHarmony(editData.element);
      if (!harmony->parent())
            return;
      Segment* segment = harmony->getParentSeg();
      if (!segment)
            return;

      const Fraction tick = segment->tick();
      const TimeSig* staffTimeSig = harmony->staff()->timeSig(tick);
      const Fraction signature = staffTimeSig ? staffTimeSig->sig() : _score->tick2measure(tick)->timesig();
      if (signature.denominator() > 0)
            harmonyTicksTab(Fraction(1, signature.denominator()) / harmony->staff()->timeStretch(tick));
      }

void ScoreView::harmonyMoveToChordRest(bool back)
      {
      Harmony* harmony = toHarmony(editData.element);
      if (!harmony->parent())
            return;
      Segment* start = harmony->getParentSeg();
      if (!start)
            return;

      if (start->measure()->isMMRest()) {
            Measure* measure = _score->tick2measure(start->tick());
            start = measure->findSegment(SegmentType::ChordRest, start->tick());
            if (!start)
                  return;
            }

      const int firstTrack = harmony->staffIdx() * VOICES;
      Segment* segment = start;
      while ((segment = back ? segment->prev1(SegmentType::ChordRest) : segment->next1(SegmentType::ChordRest))) {
            if (!segment->enabled())
                  continue;
            for (int track = firstTrack; track < firstTrack + VOICES; ++track) {
                  Element* element = segment->element(track);
                  if (element && element->isChordRest() && !(element->isRest() && toRest(element)->isGap())) {
                        harmonyEditAt(segment->measure(), segment->tick());
                        return;
                        }
                  }
            }
      }

//---------------------------------------------------------
//   harmonyBeatsTab
//    manages [;:], moving forward or back to the next beat
//    and Space/Shift-Space, to stop at next note, rest, harmony or beat.
//---------------------------------------------------------

void ScoreView::harmonyBeatsTab(bool noterest, bool back)
      {
      Harmony* harmony = toHarmony(editData.element);
      if (!harmony->parent() || !harmony->parent()->isSegment()) {
            qDebug("no segment parent");
            return;
            }
      int track        = harmony->track();
      HarmonyType ht   = harmony->harmonyType();
      Tid tid          = harmony->tid();
      Segment* segment = toSegment(harmony->parent());
      if (!segment) {
            qDebug("no segment");
            return;
            }
      Measure* measure = segment->measure();
      Fraction tick = segment->tick();

      if (back && tick == measure->tick()) {
            // previous bar, if any
            measure = measure->prevMeasure();
            if (!measure) {
                  qDebug("no previous measure");
                  return;
                  }
            }

      Fraction f = measure->ticks();
      int ticksPerBeat   = f.ticks() / ((f.numerator()>3 && (f.numerator()%3)==0 && f.denominator()>4) ? f.numerator()/3 : f.numerator());
      Fraction tickInBar = tick - measure->tick();
      Fraction newTick   = measure->tick()
                           + Fraction::fromTicks((
                              (tickInBar.ticks() + (back? -1 : ticksPerBeat)) / ticksPerBeat
                              )
                              * ticksPerBeat);

      changeState(ViewState::NORMAL);

      _score->startCmd();
      // look for next/prev beat, note, rest or chord
      for (;;) {
            segment = back ? segment->prev1(SegmentType::ChordRest) : segment->next1(SegmentType::ChordRest);

            if (!segment || (back ? (segment->tick() < newTick) : (segment->tick() > newTick))) {
                  // no segment or moved past the beat - create new segment
                  if (!back && newTick >= measure->tick() + f) {
                        // next bar, if any
                        measure = measure->nextMeasure();
                        if (!measure) {
                              qDebug("no next measure");
                              return;
                              }
                        }
                  segment = new Segment(measure, SegmentType::ChordRest, newTick - measure->tick());
                  if (!segment) {
                        qDebug("no prev segment");
                        return;
                        }
                  _score->undoAddElement(segment);
                  break;
                  }

            if (segment->tick() == newTick)
                  break;

            if (noterest) {
                  int minTrack = (track / VOICES ) * VOICES;
                  int maxTrack = minTrack + (VOICES-1);
                  if (segment->hasAnnotationOrElement(ElementType::HARMONY, minTrack, maxTrack))
                        break;
                  }
            }

      // search for next chord name
      harmony = 0;
      for (Element* e : segment->annotations()) {
            if (e->isHarmony() && e->track() == track && toHarmony(e)->tid() == tid) {
                  Harmony* h = toHarmony(e);
                  harmony = h;
                  break;
                  }
            }

      if (!harmony) {
            harmony = new Harmony(_score);
            harmony->setTrack(track);
            harmony->setParent(segment);
            harmony->setHarmonyType(ht);
            _score->undoAddElement(harmony);
            }
      _score->endCmd();

      _score->select(harmony, SelectType::SINGLE, 0);
      startEdit(harmony, Grip::NO_GRIP);

      adjustCanvasPosition(harmony, false);
      TextCursor* cursor = harmony->cursor(editData);
      cursor->moveCursorToEnd();
      _score->update();
      }

//---------------------------------------------------------
//   harmonyTicksTab
//    manages [Ctrl] [1]-[9], moving forward the given number of ticks
//---------------------------------------------------------

void ScoreView::harmonyTicksTab(const Fraction& ticks)
      {
      Harmony* harmony = toHarmony(editData.element);
      if (!harmony->parent())
            return;
      Segment* segment = harmony->getParentSeg();
      if (!segment)
            return;

      const Fraction newTick = segment->tick() + ticks;
      Measure* measure = _score->tick2measure(segment->tick());
      while (measure && newTick >= measure->endTick())
            measure = measure->nextMeasure();
      if (measure)
            harmonyEditAt(measure, newTick);
      }

void ScoreView::harmonyEditAt(Measure* measure, const Fraction& tick)
      {
      Harmony* current = toHarmony(editData.element);
      const int track = current->track();
      const HarmonyType type = current->harmonyType();
      const Tid tid = current->tid();

      changeState(ViewState::NORMAL);

      Segment* segment = measure->findSegment(SegmentType::ChordRest, tick);
      Harmony* harmony = nullptr;
      if (segment) {
            for (Element* annotation : segment->annotations()) {
                  Harmony* candidate = annotation->isHarmony() ? toHarmony(annotation) : nullptr;
                  if (annotation->isFretDiagram())
                        candidate = toFretDiagram(annotation)->harmony();
                  if (candidate && candidate->track() == track && candidate->tid() == tid) {
                        harmony = candidate;
                        break;
                        }
                  }
            }

      if (!harmony) {
            _score->startCmd();
            segment = measure->undoGetSegment(SegmentType::ChordRest, tick);
            harmony = new Harmony(_score);
            harmony->setTrack(track);
            harmony->setParent(segment);
            harmony->setHarmonyType(type);
            _score->undoAddElement(harmony);
            _score->endCmd();
            }

      if (_score->styleB(Sid::createMultiMeasureRests) && measure->mmRest()) {
            for (ScoreElement* linked : harmony->linkList()) {
                  Harmony* copy = toHarmony(linked);
                  if (copy->findMeasure() == measure->mmRest()) {
                        harmony = copy;
                        break;
                        }
                  }
            }
      _score->select(harmony, SelectType::SINGLE, 0);
      startEdit(harmony, Grip::NO_GRIP);
      adjustCanvasPosition(harmony, false);
      harmony->cursor(editData)->moveCursorToEnd();
      _score->update();
      }

}
