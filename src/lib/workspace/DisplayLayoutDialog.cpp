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

DisplayLayoutDialog::DisplayLayoutDialog(const QJsonObject &state, QWidget *parent)
    : QDialog(parent),
      m_state(state),
      m_scene(new QGraphicsScene(this)),
      m_view(new QGraphicsView(m_scene, this)),
      m_enabled(new QCheckBox(tr("Arrange and route each monitor separately"), this)),
      m_selection(new QLabel(this)),
      m_x(new QDoubleSpinBox(this)),
      m_y(new QDoubleSpinBox(this)),
      m_width(new QDoubleSpinBox(this)),
      m_height(new QDoubleSpinBox(this))
{
  setWindowTitle(tr("Individual monitor layout"));
  resize(1050, 680);
  auto *layout = new QVBoxLayout(this);
  auto *instructions = new QLabel(
      tr("Drag each monitor to match your desk. Touching edges connect; colours identify computers. "
         "You can place a Mac monitor between two Windows monitors. This does not change system display settings."),
      this
  );
  instructions->setWordWrap(true);
  layout->addWidget(instructions);
  m_enabled->setChecked(state.value("enabled").toBool());
  layout->addWidget(m_enabled);
  m_view->setRenderHint(QPainter::Antialiasing);
  m_view->setDragMode(QGraphicsView::RubberBandDrag);
  m_view->setMinimumHeight(360);
  layout->addWidget(m_view, 1);
  auto *row = new QHBoxLayout;
  auto *identify = new QPushButton(tr("Identify selected"), this);
  auto *fit = new QPushButton(tr("Fit monitors"), this);
  auto *arrangeButton = new QPushButton(tr("Arrange in a row"), this);
  row->addWidget(identify);
  row->addWidget(fit);
  row->addWidget(arrangeButton);
  row->addStretch();
  layout->addLayout(row);
  layout->addWidget(m_selection);
  auto *coordinates = new QHBoxLayout;
  const auto addCoordinate = [&](const QString &label, QDoubleSpinBox *spin, bool size) {
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
  layout->addLayout(coordinates);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, &DisplayLayoutDialog::save);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_scene, &QGraphicsScene::selectionChanged, this, &DisplayLayoutDialog::updateSelection);
  connect(fit, &QPushButton::clicked, this, &DisplayLayoutDialog::fitDisplays);
  connect(arrangeButton, &QPushButton::clicked, this, &DisplayLayoutDialog::arrange);
  connect(identify, &QPushButton::clicked, this, [this] {
    for (auto *item : m_scene->selectedItems()) {
      if (auto *display = dynamic_cast<DisplayItem *>(item)) {
        const auto object = display->display();
        Q_EMIT requestIdentify(object.value("device").toString(), object.value("id").toString());
      }
    }
  });
  loadItems();
  QTimer::singleShot(0, this, &DisplayLayoutDialog::fitDisplays);
}

void DisplayLayoutDialog::loadItems()
{
  m_loading = true;
  m_scene->clear();
  QHash<QString, int> numbers;
  for (const auto &value : m_state.value("displays").toArray()) {
    const auto display = value.toObject();
    const int number = ++numbers[display.value("device").toString()];
    m_scene->addItem(new DisplayItem(display, number, [this] { updateSelection(); }));
  }
  m_loading = false;
  updateSelection();
}

void DisplayLayoutDialog::fitDisplays()
{
  const auto bounds = m_scene->itemsBoundingRect();
  if (!bounds.isEmpty()) {
    m_scene->setSceneRect(bounds.adjusted(-200, -200, 200, 200));
    m_view->fitInView(m_scene->sceneRect(), Qt::KeepAspectRatio);
  }
}

void DisplayLayoutDialog::updateSelection()
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

void DisplayLayoutDialog::editSelection()
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
}

void DisplayLayoutDialog::arrange()
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
}

void DisplayLayoutDialog::save()
{
  QJsonArray array;
  QList<QRectF> occupied;
  for (auto *item : m_scene->items()) {
    auto *display = dynamic_cast<DisplayItem *>(item);
    if (!display)
      continue;
    const auto rectangle = display->rect().translated(display->pos());
    if (m_enabled->isChecked() && display->display().value("connected").toBool(true)) {
      for (const auto &other : occupied) {
        const auto intersection = rectangle.intersected(other);
        if (intersection.width() > 2 && intersection.height() > 2) {
          QMessageBox::warning(
              this, tr("Overlapping monitors"),
              tr("Move the monitors so their interiors do not overlap. Touching edges are allowed.")
          );
          return;
        }
      }
      occupied.append(rectangle);
    }
    array.append(display->display());
  }
  auto result = m_state;
  result["version"] = 1;
  result["enabled"] = m_enabled->isChecked();
  result["displays"] = array;
  Q_EMIT saved(result);
  accept();
}

} // namespace deskflow::workspace
