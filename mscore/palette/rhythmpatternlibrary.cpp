#include "paletteworkspace.h"
#include "palettemodel.h"
#include "musescore.h"
#include "libmscore/clef.h"
#include "libmscore/measure.h"
#include "libmscore/page.h"
#include "libmscore/part.h"
#include "libmscore/rest.h"
#include "libmscore/score.h"
#include "libmscore/segment.h"
#include "libmscore/sig.h"
#include "libmscore/staff.h"
#include "libmscore/stafftype.h"
#include "libmscore/stringdata.h"
#include "libmscore/timesig.h"
#include "libmscore/undo.h"
#include "libmscore/xml.h"
#include "thirdparty/qzip/qzipreader_p.h"

#include <QBuffer>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QImage>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSaveFile>
#include <QSet>
#include <QSignalBlocker>
#include <QtMath>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace Ms {
namespace {

struct Pattern {
      QString name;
      QString meter;
      QByteArray staffList;
      QByteArray staffType;
      QByteArray strings;
      int voice = -1;
      QImage preview;
      QString previewError;

      QStringList bars() const { return meter.split(';'); }

      QString timeSignatures() const
            {
            QStringList signatures;
            for (const QString& bar : bars()) {
                  const QString signature = bar.section(':', 0, 0);
                  if (signatures.isEmpty() || signatures.last() != signature)
                        signatures.append(signature);
                  }
            return signatures.join(QStringLiteral(" → "));
            }

      QByteArray key() const
            {
            return QCryptographicHash::hash(staffList + '\0' + meter.toUtf8() + '\0' + QByteArray::number(voice),
                                            QCryptographicHash::Sha256).toHex();
            }

      bool valid() const { return voice >= 0 && voice < VOICES && !staffList.isEmpty() && !meter.isEmpty(); }
      };

static QImage renderPattern(Pattern& pattern)
      {
      pattern.previewError = QCoreApplication::translate("RhythmPatternLibrary", "The pattern data is invalid.");
      if (!pattern.valid() || pattern.bars().size() > 4)
            return {};

      MasterScore score(MScore::defaultStyle());
      score.setMscVersion(302);
      score.setLayoutMode(LayoutMode::LINE);
      score.setStyleValue(Sid::createMultiMeasureRests, false);
      auto* part = new Part(&score);
      auto* staff = new Staff(&score);
      staff->setPart(part);
      score.appendPart(part);
      score.insertStaff(staff, 0);

      if (!pattern.staffType.isEmpty()) {
            XmlReader reader(pattern.staffType);
            if (reader.readNextStartElement() && reader.name() == "StaffType") {
                  StaffType type;
                  type.read(reader);
                  if (type.group() == StaffGroup::TAB) {
                        type.setStemless(false);
                        type.setGenDurations(false);
                        }
                  staff->setStaffType(Fraction(), type);
                  }
            }
      else if (pattern.staffList.contains("<string>") && pattern.staffList.contains("<fret>")) {
            StaffType type(*StaffType::preset(StaffTypes::TAB_DEFAULT));
            type.setStemless(false);
            type.setGenDurations(false);
            staff->setStaffType(Fraction(), type);
            }

      const bool tab = staff->isTabStaff(Fraction());
      if (tab) {
            StringData strings;
            if (!pattern.strings.isEmpty()) {
                  XmlReader reader(pattern.strings);
                  if (reader.readNextStartElement() && reader.name() == "StringData")
                        strings.read(reader);
                  }
            if (!strings.strings()) {
                  int tuning[] = {40, 45, 50, 55, 59, 64};
                  strings = StringData(24, 6, tuning);
                  }
            part->instrument()->setStringData(strings);
            }

      Fraction tick;
      Fraction previousSig(0, 1);
      for (const QString& bar : pattern.bars()) {
            const Fraction sig = Fraction::fromString(bar.section(':', 0, 0));
            const Fraction length = Fraction::fromString(bar.section(':', 1, 1));
            if (!sig.isValid() || !length.isValid() || sig <= Fraction() || length <= Fraction())
                  return {};
            auto* measure = new Measure(&score);
            measure->setTick(tick);
            measure->setTimesig(sig);
            measure->setTicks(length);
            score.measures()->add(measure);
            if (tab && tick.isZero()) {
                  auto* clef = new Clef(&score);
                  clef->setTrack(0);
                  clef->setClefType(staff->staffType(tick)->lines() <= 4 ? ClefType::TAB4 : ClefType::TAB);
                  measure->getSegment(SegmentType::HeaderClef, tick)->add(clef);
                  }
            if (!sig.identical(previousSig)) {
                  auto* timeSig = new TimeSig(&score);
                  timeSig->setSig(sig);
                  timeSig->setTrack(0);
                  measure->getSegment(SegmentType::TimeSig, tick)->add(timeSig);
                  previousSig = sig;
                  }
            score.sigmap()->add(tick.ticks(), SigEvent(length, sig));
            if (pattern.voice != 0) {
                  auto* rest = new Rest(&score);
                  rest->setDurationType(TDuration(TDuration::DurationType::V_MEASURE));
                  rest->setTicks(length);
                  rest->setTrack(0);
                  rest->setVisible(false);
                  measure->getSegment(SegmentType::ChordRest, tick)->add(rest);
                  }
            tick += length;
            }

      Segment* start = score.firstMeasure()->getSegment(SegmentType::ChordRest, Fraction());
      XmlReader reader(pattern.staffList);
      reader.setPasteMode(true);
      // Keep undoAddCR usable while reading into this disposable score.
      // Do not dispatch an application command or touch the live score.
      const MsError previousError = MScore::_error;
      MScore::setError(MS_NO_ERROR);
      score.undoStack()->beginMacro(&score);
      const bool ok = score.pasteStaff(reader, start, 0, Fraction(1, 1), pattern.voice)
                      && MScore::_error == MS_NO_ERROR;
      score.undoStack()->endMacro(false);
      MScore::setError(previousError);
      if (!ok) {
            pattern.previewError = QCoreApplication::translate("RhythmPatternLibrary",
                  "The preview score could not read the pattern (line %1, element %2).")
                  .arg(reader.lineNumber()).arg(reader.name().toString());
            return {};
            }

      pattern.previewError = QCoreApplication::translate("RhythmPatternLibrary", "The preview has no drawable notation.");
      score.doLayout();
      score.setPrinting(true);
      QList<Element*> elements;
      for (Page* page : score.pages())
            elements.append(page->elements());
      if (elements.isEmpty())
            return {};
      std::stable_sort(elements.begin(), elements.end(), elementLessThan);
      QRectF bounds;
      for (const Element* element : elements) {
            if (element->visible() && !element->isPage() && !element->isSystem() && !element->isMeasure())
                  bounds |= element->bbox().translated(element->pagePos());
            }
      if (bounds.isEmpty())
            return {};
      bounds.adjust(-8, -8, 8, 8);
      const qreal scale = qMin(3.0, 1600.0 / qMax(bounds.width(), bounds.height()));
      QImage image(QSize(qCeil(bounds.width() * scale), qCeil(bounds.height() * scale)), QImage::Format_ARGB32_Premultiplied);
      image.fill(Qt::white);
      QPainter painter(&image);
      painter.setRenderHint(QPainter::Antialiasing);
      painter.setRenderHint(QPainter::TextAntialiasing);
      painter.scale(scale, scale);
      painter.translate(-bounds.topLeft());
      for (const Element* element : elements) {
            if (element->visible())
                  element->drawAt(&painter, element->pagePos());
            }
      painter.end();
      pattern.previewError.clear();
      return image;
      }

class Preview : public QWidget {
      QImage image;
   public:
      explicit Preview(const QImage& image = QImage(), QWidget* parent = nullptr)
         : QWidget(parent), image(image)
            {
            setAttribute(Qt::WA_TransparentForMouseEvents);
            }
      void setImage(const QImage& value) { image = value; update(); }
      void paintEvent(QPaintEvent*) override
            {
            QPainter painter(this);
            if (image.isNull()) {
                  painter.drawText(rect(), Qt::AlignCenter, tr("Preview unavailable"));
                  return;
                  }
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            const QSize size = image.size().scaled(rect().adjusted(4, 0, -4, 0).size(), Qt::KeepAspectRatio);
            painter.drawImage(QRect(QPoint(4, (height() - size.height()) / 2), size), image);
            }
      };

class Library {
      QString path = QDir(dataPath).filePath("rhythm-pattern-library.json");
   public:
      QVector<Pattern> patterns;
      QSet<QByteArray> imported;
      bool load()
            {
            QFile file(path);
            if (!file.exists())
                  return true;
            if (!file.open(QIODevice::ReadOnly))
                  return false;
            const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
            const QJsonObject root = document.object();
            if (root.value("version").toInt() != 1 || !root.value("patterns").isArray()
                || !root.value("imported").isArray())
                  return false;
            for (const QJsonValue& item : root.value("patterns").toArray()) {
                  const QJsonObject row = item.toObject();
                  Pattern p;
                  p.name = row.value("name").toString();
                  p.meter = row.value("meter").toString();
                  p.staffList = QByteArray::fromBase64(row.value("staffList").toString().toLatin1());
                  p.staffType = QByteArray::fromBase64(row.value("staffType").toString().toLatin1());
                  p.strings = QByteArray::fromBase64(row.value("strings").toString().toLatin1());
                  p.voice = row.value("voice").toInt(-1);
                  if (!p.valid())
                        return false;
                  patterns.append(p);
                  }
            for (const QJsonValue& item : root.value("imported").toArray())
                  imported.insert(item.toString().toLatin1());
            return true;
            }

      bool save() const
            {
            QJsonArray rows;
            for (const Pattern& p : patterns) {
                  rows.append(QJsonObject{{"name", p.name}, {"meter", p.meter}, {"voice", p.voice},
                        {"staffList", QString::fromLatin1(p.staffList.toBase64())},
                        {"staffType", QString::fromLatin1(p.staffType.toBase64())},
                        {"strings", QString::fromLatin1(p.strings.toBase64())}});
                  }
            QJsonArray keys;
            for (const QByteArray& key : imported)
                  keys.append(QString::fromLatin1(key));
            const QByteArray data = QJsonDocument(QJsonObject{{"version", 1}, {"patterns", rows}, {"imported", keys}}).toJson();
            QSaveFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
            }

      void import(Pattern p)
            {
            if (!p.valid())
                  return;
            const QByteArray key = p.key();
            if (imported.contains(key))
                  return;
            imported.insert(key);
            if (std::none_of(patterns.begin(), patterns.end(), [&](const Pattern& existing) { return existing.key() == key; }))
                  patterns.append(std::move(p));
            }
      };

static void importLegacy(Library& library, PaletteTreeModel* model)
      {
      if (model) {
            for (int row = 0; row < model->rowCount(); ++row) {
                  const QModelIndex panel = model->index(row, 0);
                  for (int col = 0; col < model->rowCount(panel); ++col) {
                        const auto cell = model->findCell(model->index(col, 0, panel));
                        if (cell)
                              library.import({cell->name, cell->rhythmMeter, cell->rhythmPattern, {}, {}, cell->rhythmVoice, {}, {}});
                        }
                  }
            }
      const QDir dir(QDir(dataPath).filePath("workspaces"));
      for (const QString& file : dir.entryList({"*.workspace"}, QDir::Files)) {
            MQZipReader archive(dir.filePath(file));
            XmlReader xml(archive.fileData("workspace.xml"));
            QString cellName;
            while (!xml.atEnd()) {
                  xml.readNext();
                  if (xml.isEndElement() && xml.name() == "Cell")
                        cellName.clear();
                  if (!xml.isStartElement())
                        continue;
                  if (xml.name() == "Cell")
                        cellName = xml.attribute("name");
                  else if (xml.name() == "rhythmPattern" && !cellName.isEmpty()) {
                        const int voice = xml.intAttribute("voice", -1);
                        const QString meter = xml.attribute("meter");
                        library.import({cellName, meter, QByteArray::fromBase64(xml.readElementText().trimmed().toLatin1()), {}, {}, voice, {}, {}});
                        }
                  }
            }
      }

} // namespace

void PaletteWorkspace::showRhythmPatternLibrary()
      {
      Score* score = mscore->currentScore();
      Library library;
      if (!library.load()) {
            QMessageBox::warning(mscore, tr("Rhythm Pattern Library"), tr("The saved library could not be read."));
            return;
            }
      const auto imported = library.imported;
      importLegacy(library, userPaletteModel());
      if (library.imported != imported && !library.save())
            QMessageBox::warning(mscore, tr("Rhythm Pattern Library"), tr("Imported patterns could not be saved."));

      Pattern selected;
      if (score) {
            selected.staffList = score->rhythmPattern(&selected.meter, &selected.voice);
            if (selected.valid()) {
                  QBuffer buffer(&selected.staffType);
                  buffer.open(QIODevice::WriteOnly);
                  XmlWriter writer(score, &buffer);
                  const Fraction tick = score->selection().tickStart();
                  const Staff* staff = score->staff(score->selection().staffStart());
                  staff->staffType(tick)->write(writer);
                  writer.flush();
                  if (staff->isTabStaff(tick)) {
                        QBuffer tuning(&selected.strings);
                        tuning.open(QIODevice::WriteOnly);
                        XmlWriter tuningWriter(score, &tuning);
                        staff->part()->instrument(tick)->stringData()->write(tuningWriter);
                        tuningWriter.flush();
                        }
                  }
            }

      QDialog dialog(mscore);
      dialog.setWindowTitle(tr("Rhythm Pattern Library"));
      dialog.setSizeGripEnabled(true);
      dialog.resize(1080, 730);
      dialog.setMinimumSize(750, 500);
      auto* layout = new QVBoxLayout(&dialog);
      auto* registration = new QHBoxLayout;
      auto* name = new QLineEdit(&dialog);
      name->setPlaceholderText(tr("Pattern name"));
      auto* add = new QPushButton(tr("Register Selection"), &dialog);
      name->setEnabled(selected.valid());
      add->setEnabled(selected.valid());
      registration->addWidget(name, 1);
      registration->addWidget(add);
      layout->addLayout(registration);
      if (selected.valid()) {
            selected.preview = renderPattern(selected);
            auto* preview = new Preview(selected.preview, &dialog);
            preview->setAttribute(Qt::WA_TransparentForMouseEvents, false);
            preview->setToolTip(selected.previewError);
            preview->setMinimumHeight(100);
            layout->addWidget(preview);
            }

      auto* filters = new QHBoxLayout;
      auto* bars = new QComboBox(&dialog);
      auto* meter = new QComboBox(&dialog);
      bars->addItem(tr("All measure counts"), 0);
      for (int i = 1; i <= 4; ++i)
            bars->addItem(tr("%n measure(s)", nullptr, i), i);
      filters->addWidget(bars);
      filters->addWidget(meter);
      filters->addStretch();
      layout->addLayout(filters);

      auto* table = new QTableWidget(0, 4, &dialog);
      table->setHorizontalHeaderLabels({tr("Name"), tr("Measures"), tr("Time signature"), tr("Score preview")});
      table->verticalHeader()->hide();
      table->verticalHeader()->setDefaultSectionSize(90);
      table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
      table->setColumnWidth(0, 155);
      table->setSelectionBehavior(QAbstractItemView::SelectRows);
      table->setSelectionMode(QAbstractItemView::SingleSelection);
      table->setEditTriggers(QAbstractItemView::NoEditTriggers);
      layout->addWidget(table, 1);

      auto* buttons = new QHBoxLayout;
      auto button = [&](const QString& text) {
            auto* widget = new QPushButton(text, &dialog);
            widget->setAutoDefault(false);
            buttons->addWidget(widget);
            return widget;
            };
      auto* rename = button(tr("Rename"));
      auto* remove = button(tr("Delete"));
      auto* up = button(tr("Move Up"));
      auto* down = button(tr("Move Down"));
      buttons->addStretch();
      auto* apply = button(tr("Apply"));
      auto* close = button(tr("Close"));
      layout->addLayout(buttons);

      auto current = [&]() {
            const auto* item = table->item(table->currentRow(), 0);
            return item ? item->data(Qt::UserRole).toInt() : -1;
            };
      auto updateButtons = [&]() {
            const int i = current();
            rename->setEnabled(i >= 0);
            remove->setEnabled(i >= 0);
            up->setEnabled(i > 0);
            down->setEnabled(i >= 0 && i + 1 < library.patterns.size());
            apply->setEnabled(i >= 0 && score && score->selection().isRange());
            };
      auto refresh = [&](int keep) {
            const QString filter = meter->currentData().toString();
            const int count = bars->currentData().toInt();
            const QSignalBlocker meterSignals(meter);
            meter->clear();
            meter->addItem(tr("All time signatures"), QString());
            for (const Pattern& p : qAsConst(library.patterns)) {
                  const QString label = p.timeSignatures();
                  if (meter->findData(label) < 0)
                        meter->addItem(label, label);
                  }
            meter->setCurrentIndex(qMax(0, meter->findData(filter)));
            table->setRowCount(0);
            for (int i = 0; i < library.patterns.size(); ++i) {
                  Pattern& p = library.patterns[i];
                  if ((count && p.bars().size() != count)
                      || (!meter->currentData().toString().isEmpty() && p.timeSignatures() != filter))
                        continue;
                  const int row = table->rowCount();
                  table->insertRow(row);
                  auto* item = new QTableWidgetItem(p.name);
                  item->setData(Qt::UserRole, i);
                  table->setItem(row, 0, item);
                  table->setItem(row, 1, new QTableWidgetItem(QString::number(p.bars().size())));
                  table->setItem(row, 2, new QTableWidgetItem(p.timeSignatures()));
                  if (p.preview.isNull())
                        p.preview = renderPattern(p);
                  if (p.preview.isNull()) {
                        auto* unavailable = new QTableWidgetItem(p.previewError);
                        unavailable->setToolTip(p.previewError);
                        table->setItem(row, 3, unavailable);
                        }
                  else
                        table->setCellWidget(row, 3, new Preview(p.preview, table));
                  if (i == keep)
                        table->selectRow(row);
                  }
            updateButtons();
            };
      auto persist = [&](const QVector<Pattern>& previous, int keep) {
            if (!library.save()) {
                  library.patterns = previous;
                  QMessageBox::warning(&dialog, tr("Rhythm Pattern Library"), tr("The library could not be saved."));
                  }
            refresh(keep);
            };
      connect(table, &QTableWidget::currentCellChanged, &dialog, updateButtons);
      connect(bars, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [&](int) { refresh(current()); });
      connect(meter, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [&](int) { refresh(current()); });
      connect(add, &QPushButton::clicked, &dialog, [&]() {
            const QString value = name->text().trimmed();
            if (value.isEmpty()) {
                  name->setFocus();
                  return;
                  }
            const auto previous = library.patterns;
            selected.name = value;
            library.patterns.append(selected);
            persist(previous, library.patterns.size() - 1);
            });
      connect(rename, &QPushButton::clicked, &dialog, [&]() {
            const int i = current();
            if (i < 0)
                  return;
            bool accepted = false;
            const QString value = QInputDialog::getText(&dialog, tr("Rename Pattern"), tr("Pattern name:"),
                  QLineEdit::Normal, library.patterns[i].name, &accepted).trimmed();
            if (accepted && !value.isEmpty()) {
                  const auto previous = library.patterns;
                  library.patterns[i].name = value;
                  persist(previous, i);
                  }
            });
      connect(remove, &QPushButton::clicked, &dialog, [&]() {
            const int i = current();
            if (i >= 0 && QMessageBox::question(&dialog, tr("Delete Pattern"), tr("Delete the selected pattern?"))
                == QMessageBox::Yes) {
                  const auto previous = library.patterns;
                  library.patterns.removeAt(i);
                  persist(previous, -1);
                  }
            });
      auto move = [&](int delta) {
            const int i = current();
            const int j = i + delta;
            if (i >= 0 && j >= 0 && j < library.patterns.size()) {
                  const auto previous = library.patterns;
                  qSwap(library.patterns[i], library.patterns[j]);
                  persist(previous, j);
                  }
            };
      connect(up, &QPushButton::clicked, &dialog, [&]() { move(-1); });
      connect(down, &QPushButton::clicked, &dialog, [&]() { move(1); });
      auto applyPattern = [&]() {
            const int i = current();
            if (i < 0 || !score || mscore->currentScore() != score)
                  return;
            const Pattern& p = library.patterns.at(i);
            QString error;
            if (score->applyRhythmPattern(p.staffList, p.meter, p.voice, &error))
                  dialog.accept();
            else
                  QMessageBox::information(&dialog, tr("Rhythm Pattern Library"), error);
            };
      connect(apply, &QPushButton::clicked, &dialog, applyPattern);
      connect(table, &QTableWidget::cellDoubleClicked, &dialog, applyPattern);
      connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
      refresh(-1);
      dialog.exec();
      disconnect(table, nullptr, &dialog, nullptr);
      }

} // namespace Ms
