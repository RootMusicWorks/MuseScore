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

#ifndef __REHEARSALMARK_H__
#define __REHEARSALMARK_H__

#include "systemtext.h"

namespace Ms {

class Staff;

//---------------------------------------------------------
//   @@ RehearsalMark
//---------------------------------------------------------

class RehearsalMark final : public TextBase  {
      enum class FramePlacement { None = 0, Legacy = 1, Offset = 2, Automatic = 3 };
      FramePlacement _framePlacement { FramePlacement::None };
      int _frameTabStaff { -1 }; // Index in the master score.

   public:
      RehearsalMark(Score* score);
      void setNewInsertionDefaults();
      Spatium setupAutomaticFrame(Measure* selectedMeasure, int firstStaff, int endStaff);
      void layoutFrameOnTabStaff();
      bool usesAutomaticFrame() const { return _framePlacement == FramePlacement::Automatic; }
      Staff* frameTabStaff() const;
      QRectF frameBounds() const;
      bool readProperties(XmlReader&) override;
      void write(XmlWriter&) const override;

      RehearsalMark* clone() const override { return new RehearsalMark(*this); }
      ElementType type() const override     { return ElementType::REHEARSAL_MARK; }
      Segment* segment() const              { return (Segment*)parent(); }
      void layout() override;
      QVariant propertyDefault(Pid id) const override;
      };


}     // namespace Ms
#endif

