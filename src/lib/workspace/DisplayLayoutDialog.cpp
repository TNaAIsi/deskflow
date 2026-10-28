/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "DisplayLayoutDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace deskflow::workspace {
namespace {

class DisplayItem : public QGraphicsRectItem
{
public:
  DisplayItem(const QJsonObject &display, int number, std::function<void()> changed)
      : m_display(display),
        m_changed(std::move(changed))
  {
    const auto canvas = display.value("canvas").toObject();
    setRect(0, 0, canvas.value("width").toDouble(1920), canvas.value("height").toDouble(1080));
    setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
    const auto color = QColor::fromHsv(static_cast<int>(qHash(display.value("device").toString()) % 360), 110, 220);
    setBrush(display.value("connected").toBool(true) ? color : QColor(125, 125, 125));
    setPen(QPen(color.darker(160), 5));
    m_label = new QGraphicsTextItem(this);
    m_label->setDefaultTextColor(Qt::black);
    m_label->setTextWidth(rect().width() - 32);
    QFont font;
    font.setPixelSize(qRound(std::clamp(rect().width() / 16.0, 28.0, 96.0)));
    font.setBold(true);
    m_label->setFont(font);
    m_label->setPlainText(
        QString("%1 · %2\n%3%4")
            .arg(display.value("device").toString())
            .arg(number)
            .arg(display.value("label").toString())
            .arg(display.value("connected").toBool(true) ? QString() : QString("\n") + QObject::tr("Disconnected"))
    );
    m_label->setPos(20, 16);
    setPos(canvas.value("x").toDouble(), canvas.value("y").toDouble());
  }

  QJsonObject display() const
  {
    auto result = m_display;
    result["canvas"] =
        QJsonObject{{"x", pos().x()}, {"y", pos().y()}, {"width", rect().width()}, {"height", rect().height()}};
    return result;
  }

  void setCanvasSize(qreal width, qreal height)
  {
    setRect(0, 0, width, height);
    m_label->setTextWidth(std::max(1.0, width - 32));
    auto font = m_label->font();
    font.setPixelSize(qRound(std::clamp(width / 16.0, 28.0, 96.0)));
    m_label->setFont(font);
  }

  void setSnapping(bool enabled)
  {
    m_snapping = enabled;
  }

protected:
  QVariant itemChange(GraphicsItemChange change, const QVariant &value) override
  {
    if (change == ItemPositionChange && scene() && m_snapping) {
      auto position = value.toPointF();
      constexpr qreal snap = 40;
      for (auto *other : scene()->items()) {
        auto *display = dynamic_cast<DisplayItem *>(other);
        if (!display || display == this)
          continue;
        const auto bounds = display->rect().translated(display->pos());
        const qreal horizontal[] = {
            bounds.left(), bounds.right(), bounds.left() - rect().width(), bounds.right() - rect().width()
        };
        const qreal vertical[] = {
            bounds.top(), bounds.bottom(), bounds.top() - rect().height(), bounds.bottom() - rect().height()
        };
        for (qreal x : horizontal)
          if (qAbs(position.x() - x) < snap)
            position.setX(x);
        for (qreal y : vertical)
          if (qAbs(position.y() - y) < snap)
            position.setY(y);
      }
      return position;
    }
    if (change == ItemPositionHasChanged && m_changed)
      m_changed();
    return QGraphicsRectItem::itemChange(change, value);
  }

private:
  QJsonObject m_display;
  QGraphicsTextItem *m_label = nullptr;
  std::function<void()> m_changed;
  bool m_snapping = true;
};

} // namespace

DisplayLayoutWidget::DisplayLayoutWidget(const QJsonObject &state, QWidget *parent)
    : QWidget(parent),
      m_state(state),
      m_scene(new QGraphicsScene(this)),
      m_view(new QGraphicsView(m_scene, this)),
      m_enabled(new QCheckBox(tr("Arrange and route each monitor separately"), this)),
      m_canvasControls(new QWidget(this)),
      m_selection(new QLabel(this)),
      m_saveError(new QLabel(this)),
      m_x(new QDoubleSpinBox(this)),
      m_y(new QDoubleSpinBox(this)),
      m_width(new QDoubleSpinBox(this)),
      m_height(new QDoubleSpinBox(this))
{
  setObjectName("displayLayoutEditor");
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  auto *instructions = new QLabel(
      tr("Drag each monitor to match your desk. Touching edges connect; colours identify computers. "
         "You can place a Mac monitor between two Windows monitors. This does not change system display settings."),
      this
  );
  instructions->setWordWrap(true);
  layout->addWidget(instructions);
  m_enabled->setChecked(state.value("enabled").toBool());
  m_enabled->setObjectName("independentMonitorMode");
  layout->addWidget(m_enabled);
  auto *modeHelp = new QLabel(
      tr("When enabled, this canvas controls pointer directions and the computer grid is inactive. Devices without "
         "display information stay outside automatic switching. Disable this option to use the computer grid again; "
         "your monitor positions are kept. Changes are applied with the configuration dialog Save button."),
      this
  );
  modeHelp->setWordWrap(true);
  layout->addWidget(modeHelp);
  m_saveError->setObjectName("layoutSaveError");
  m_saveError->setWordWrap(true);
  m_saveError->hide();
  layout->addWidget(m_saveError);
  auto *canvasLayout = new QVBoxLayout(m_canvasControls);
  canvasLayout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(m_canvasControls, 1);
  m_canvasControls->setVisible(m_enabled->isChecked());
  m_view->setObjectName("displayLayoutCanvas");
  m_view->setRenderHint(QPainter::Antialiasing);
  m_view->setDragMode(QGraphicsView::RubberBandDrag);
  m_view->setMinimumHeight(220);
  canvasLayout->addWidget(m_view, 1);
  auto *row = new QHBoxLayout;
  auto *identify = new QPushButton(tr("Identify selected"), this);
  auto *fit = new QPushButton(tr("Fit monitors"), this);
  auto *arrangeButton = new QPushButton(tr("Arrange in a row"), this);
  row->addWidget(identify);
  row->addWidget(fit);
  row->addWidget(arrangeButton);
  row->addStretch();
  canvasLayout->addLayout(row);
  canvasLayout->addWidget(m_selection);
  auto *coordinates = new QHBoxLayout;
  const auto addCoordinate = [&](const QString &label, QDoubleSpinBox *spin, bool size) {
    spin->setObjectName(
        label == tr("X")          ? "displayCanvasX"
        : label == tr("Y")        ? "displayCanvasY"
        : size && spin == m_width ? "displayCanvasWidth"
                                  : "displayCanvasHeight"
    );
    spin->setDecimals(0);
    spin->setRange(size ? 1 : -1000000, 1000000);
    spin->setSingleStep(10);
    coordinates->addWidget(new QLabel(label, this));
    coordinates->addWidget(spin);
    connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { editSelection(); });
  };
  addCoordinate(tr("X"), m_x, false);
  addCoordinate(tr("Y"), m_y, false);
  addCoordinate(tr("Width"), m_width, true);
  addCoordinate(tr("Height"), m_height, true);
  canvasLayout->addLayout(coordinates);
  connect(m_enabled, &QCheckBox::toggled, this, [this](bool enabled) {
    m_canvasControls->setVisible(enabled);
    edited();
    QTimer::singleShot(0, this, &DisplayLayoutWidget::fitDisplays);
  });
  connect(m_scene, &QGraphicsScene::selectionChanged, this, &DisplayLayoutWidget::updateSelection);
  connect(fit, &QPushButton::clicked, this, &DisplayLayoutWidget::fitDisplays);
  connect(arrangeButton, &QPushButton::clicked, this, &DisplayLayoutWidget::arrange);
  connect(identify, &QPushButton::clicked, this, [this] {
    for (auto *item : m_scene->selectedItems()) {
      if (auto *display = dynamic_cast<DisplayItem *>(item)) {
        const auto object = display->display();
        Q_EMIT requestIdentify(object.value("device").toString(), object.value("id").toString());
      }
    }
  });
  loadItems();
  QTimer::singleShot(0, this, &DisplayLayoutWidget::fitDisplays);
}

DisplayLayoutWidget::~DisplayLayoutWidget()
{
  // QGraphicsScene can emit selectionChanged while QWidget deletes children,
  // after this derived widget's destructor has already completed.
  disconnect(m_scene, nullptr, this, nullptr);
  m_scene->blockSignals(true);
  m_view->setScene(nullptr);
}

void DisplayLayoutWidget::loadItems()
{
  m_loading = true;
  m_scene->clear();
  QHash<QString, int> numbers;
  for (const auto &value : m_state.value("displays").toArray()) {
    const auto display = value.toObject();
    const int number = ++numbers[display.value("device").toString()];
    m_scene->addItem(new DisplayItem(display, number, [this] {
      updateSelection();
      edited();
    }));
  }
  m_loading = false;
  updateSelection();
}

void DisplayLayoutWidget::fitDisplays()
{
  const auto bounds = m_scene->itemsBoundingRect();
  if (!bounds.isEmpty()) {
    m_scene->setSceneRect(bounds.adjusted(-200, -200, 200, 200));
    m_view->fitInView(m_scene->sceneRect(), Qt::KeepAspectRatio);
  }
}

void DisplayLayoutWidget::updateSelection()
{
  if (m_loading)
    return;
  m_loading = true;
  m_scene->setSceneRect(m_scene->sceneRect().united(m_scene->itemsBoundingRect().adjusted(-200, -200, 200, 200)));
  DisplayItem *selected = nullptr;
  for (auto *item : m_scene->selectedItems()) {
    if ((selected = dynamic_cast<DisplayItem *>(item)))
      break;
  }
  for (auto *spin : {m_x, m_y, m_width, m_height})
    spin->setEnabled(selected != nullptr);
  if (selected) {
    const auto display = selected->display();
    m_selection->setText(display.value("device").toString() + " · " + display.value("label").toString());
    m_x->setValue(selected->pos().x());
    m_y->setValue(selected->pos().y());
    m_width->setValue(selected->rect().width());
    m_height->setValue(selected->rect().height());
  } else {
    m_selection->setText(tr("Select a monitor to edit its position and canvas size."));
  }
  m_loading = false;
}

void DisplayLayoutWidget::editSelection()
{
  if (m_loading)
    return;
  m_loading = true;
  for (auto *item : m_scene->selectedItems()) {
    if (auto *display = dynamic_cast<DisplayItem *>(item)) {
      display->setCanvasSize(m_width->value(), m_height->value());
      display->setSnapping(false);
      display->setPos(m_x->value(), m_y->value());
      display->setSnapping(true);
      break;
    }
  }
  m_loading = false;
  updateSelection();
  edited();
}

void DisplayLayoutWidget::arrange()
{
  auto displays = m_scene->items();
  std::sort(displays.begin(), displays.end(), [](auto *left, auto *right) {
    return left->pos().x() < right->pos().x();
  });
  m_loading = true;
  qreal x = 0;
  for (auto *item : displays) {
    if (auto *display = dynamic_cast<DisplayItem *>(item)) {
      display->setSnapping(false);
      display->setPos(x, 0);
      display->setSnapping(true);
      x += display->rect().width();
    }
  }
  m_loading = false;
  updateSelection();
  fitDisplays();
  edited();
}

namespace {
QString displayIdentity(const QJsonObject &display)
{
  return display.value("device").toString() + QChar(0x1f) + display.value("id").toString();
}
QJsonObject placements(const QJsonObject &state)
{
  QJsonObject result;
  for (const auto &value : state.value("displays").toArray()) {
    const auto display = value.toObject();
    result.insert(displayIdentity(display), display.value("canvas"));
  }
  return result;
}
} // namespace

QJsonObject DisplayLayoutWidget::layoutState() const
{
  QJsonArray array;
  for (auto *item : m_scene->items())
    if (const auto *display = dynamic_cast<DisplayItem *>(item))
      array.append(display->display());
  auto result = m_state;
  result["version"] = 1;
  result["enabled"] = m_enabled->isChecked();
  result["displays"] = array;
  return result;
}

bool DisplayLayoutWidget::independentEnabled() const
{
  return m_enabled->isChecked();
}

bool DisplayLayoutWidget::isModified() const
{
  const auto staged = layoutState();
  return staged.value("enabled").toBool() != m_state.value("enabled").toBool() ||
         placements(staged) != placements(m_state);
}

void DisplayLayoutWidget::setState(const QJsonObject &state, bool preserveEdits)
{
  const auto staged = layoutState();
  const auto originalPlacements = placements(m_state);
  const auto stagedPlacements = placements(staged);
  const bool enabledChanged = staged.value("enabled").toBool() != m_state.value("enabled").toBool();
  auto visible = state;
  auto displays = visible.value("displays").toArray();
  QSet<QString> liveKeys;
  for (qsizetype index = 0; index < displays.size(); ++index) {
    auto display = displays.at(index).toObject();
    const auto key = displayIdentity(display);
    liveKeys.insert(key);
    if (preserveEdits && stagedPlacements.contains(key) && stagedPlacements.value(key) != originalPlacements.value(key))
      display["canvas"] = stagedPlacements.value(key);
    displays[index] = display;
  }
  if (preserveEdits)
    for (const auto &value : staged.value("displays").toArray()) {
      auto display = value.toObject();
      const auto key = displayIdentity(display);
      if (!liveKeys.contains(key) && stagedPlacements.value(key) != originalPlacements.value(key)) {
        display["connected"] = false;
        displays.append(display);
      }
    }
  visible["displays"] = displays;
  m_loading = true;
  m_enabled->setChecked(
      preserveEdits && enabledChanged ? staged.value("enabled").toBool() : state.value("enabled").toBool()
  );
  m_canvasControls->setVisible(m_enabled->isChecked());
  m_state = visible;
  loadItems();
  // The baseline contains current native metadata, but never staged coordinates.
  m_state = state;
  m_loading = false;
  setSaveError({});
  QTimer::singleShot(0, this, &DisplayLayoutWidget::fitDisplays);
  Q_EMIT changed();
}

bool DisplayLayoutWidget::validate(QString *message) const
{
  QList<QRectF> occupied;
  if (!m_enabled->isChecked())
    return true;
  for (auto *item : m_scene->items()) {
    const auto *display = dynamic_cast<DisplayItem *>(item);
    if (!display || !display->display().value("connected").toBool(true))
      continue;
    const auto rectangle = display->rect().translated(display->pos());
    for (const auto &other : occupied) {
      const auto overlap = rectangle.intersected(other);
      if (overlap.width() > 2 && overlap.height() > 2) {
        if (message)
          *message = tr("Move the monitors so their interiors do not overlap. Touching edges are allowed.");
        return false;
      }
    }
    occupied.append(rectangle);
  }
  return true;
}

void DisplayLayoutWidget::setSaveError(const QString &message)
{
  m_saveError->setText(message);
  m_saveError->setVisible(!message.isEmpty());
}

void DisplayLayoutWidget::edited()
{
  if (m_loading)
    return;
  setSaveError({});
  Q_EMIT changed();
}

DisplayLayoutDialog::DisplayLayoutDialog(const QJsonObject &state, QWidget *parent)
    : QDialog(parent),
      m_editor(new DisplayLayoutWidget(state, this))
{
  setWindowTitle(tr("Individual monitor layout"));
  resize(1050, 680);
  auto *layout = new QVBoxLayout(this);
  layout->addWidget(m_editor, 1);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, &DisplayLayoutDialog::save);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_editor, &DisplayLayoutWidget::requestIdentify, this, &DisplayLayoutDialog::requestIdentify);
}

void DisplayLayoutDialog::setSaveError(const QString &message)
{
  m_saveRejected = true;
  m_editor->setSaveError(message);
}

void DisplayLayoutDialog::save()
{
  m_saveRejected = false;
  m_editor->setSaveError({});
  QString error;
  if (!m_editor->validate(&error)) {
    setSaveError(error);
    return;
  }
  Q_EMIT saved(m_editor->layoutState());
  if (!m_saveRejected)
    accept();
}

} // namespace deskflow::workspace
