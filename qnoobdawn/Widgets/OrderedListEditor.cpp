/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2018-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "OrderedListEditor.h"
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QToolButton>
#include "Code/QRDUtils.h"
#include "Code/Resources.h"

OrderedListEditor::OrderedListEditor(const QString &itemName, OrderedItemExtras extras,
                                     const CustomProp &prop, QWidget *parent)
    : RDTableWidget(parent)
{
  setFont(Formatter::PreferredFont());

  m_Prop = prop;

  setDragEnabled(true);
  setDragDropOverwriteMode(false);
  setDragDropMode(QAbstractItemView::InternalMove);
  setDefaultDropAction(Qt::MoveAction);
  setAlternatingRowColors(true);
  setSelectionMode(QAbstractItemView::SingleSelection);
  setSelectionBehavior(QAbstractItemView::SelectRows);
  setCornerButtonEnabled(false);

  horizontalHeader()->setHighlightSections(false);
  horizontalHeader()->setMinimumSectionSize(50);
  verticalHeader()->setHighlightSections(false);

  QStringList labels;
  int columnCount = 1;
  labels << itemName;

  if(!prop.name.isEmpty())
  {
    columnCount++;
    labels << prop.name;
  }

  if(extras & OrderedItemExtras::BrowseFile)
  {
    columnCount++;
    labels << tr("Browse");
    m_Extras.push_back(OrderedItemExtras::BrowseFile);
  }
  else if(extras & OrderedItemExtras::BrowseFolder)
  {
    columnCount++;
    labels << tr("Browse");
    m_Extras.push_back(OrderedItemExtras::BrowseFolder);
  }
  if(extras & OrderedItemExtras::Delete)
  {
    columnCount++;
    labels << tr("Delete");
    m_Extras.push_back(OrderedItemExtras::Delete);
  }

  setColumnCount(columnCount);
  setHorizontalHeaderLabels(labels);

  horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  for(int i = 1; i < columnCount; i++)
    horizontalHeader()->setSectionResizeMode(i, QHeaderView::ResizeToContents);

  if(!prop.name.isEmpty())
    horizontalHeaderItem(1)->setToolTip(prop.tooltip);

  QObject::connect(this, &RDTableWidget::cellChanged, this, &OrderedListEditor::cellChanged);
}

OrderedListEditor::~OrderedListEditor()
{
}

QWidget *OrderedListEditor::makeCellWidget(int col, OrderedItemExtras extra)
{
  if(extra == OrderedItemExtras::Delete)
  {
    QToolButton *ret = new QToolButton(this);
    ret->setAutoRaise(true);
    ret->setIcon(Icons::del());
    QObject::connect(ret, &QToolButton::clicked, [this, ret, col, extra]() {
      for(int row = 0; row < rowCount(); row++)
        if(cellWidget(row, col) == ret)
          return extraClicked(row, extra);

      qCritical() << "Couldn't find extra button on click";
    });

    return ret;
  }
  else if(extra == OrderedItemExtras::BrowseFile || extra == OrderedItemExtras::BrowseFolder)
  {
    QToolButton *ret = new QToolButton(this);
    ret->setAutoRaise(true);
    ret->setIcon(Icons::folder_page_white());
    QObject::connect(ret, &QToolButton::clicked, [this, ret, col, extra]() {
      for(int row = 0; row < rowCount(); row++)
        if(cellWidget(row, col) == ret)
          return extraClicked(row, extra);

      qCritical() << "Couldn't find extra button on click";
    });

    return ret;
  }
  else if(extra == OrderedItemExtras::CustomProp)
  {
    QCheckBox *ret = new QCheckBox(this);
    if(m_Prop.defaultValue)
      ret->setChecked(true);
    return ret;
  }
  else
  {
    return NULL;
  }
}

void OrderedListEditor::setItemsAndProp(const QStringList &strings, const QList<bool> &prop)
{
  setUpdatesEnabled(false);
  clearContents();

  setRowCount(strings.count());

  for(int i = 0; i < strings.count(); i++)
  {
    setItem(i, 0, new QTableWidgetItem(strings[i]));

    if(m_Prop.valid())
    {
      QWidget *w = makeCellWidget(1, OrderedItemExtras::CustomProp);
      if(i < prop.count())
      {
        QCheckBox *c = qobject_cast<QCheckBox *>(w);
        if(c)
          c->setChecked(prop[i]);
      }
      w->setToolTip(m_Prop.tooltip);

      QWidget *wrapperWidget = new QWidget();
      QHBoxLayout *l = new QHBoxLayout();
      l->setAlignment(Qt::AlignCenter);
      l->addWidget(w);
      l->setContentsMargins(QMargins(0, 0, 0, 0));
      wrapperWidget->setLayout(l);
      setCellWidget(i, 1, wrapperWidget);
    }

    for(int c = 0; c < m_Extras.size(); c++)
      setCellWidget(i, c + firstExtraColumn(), makeCellWidget(c + firstExtraColumn(), m_Extras[c]));
  }

  // if we added any strings above the new item row was automatically
  // appended. If not, add it explicitly here
  if(strings.count() == 0)
    addNewItemRow();

  resizeColumnToContents(0);
  for(int c = 0; c < m_Extras.size(); c++)
    resizeColumnToContents(c + firstExtraColumn());

  setUpdatesEnabled(true);
}

void OrderedListEditor::addNewItemRow()
{
  if(!allowAddition())
    return;

  insertRow(rowCount());

  QTableWidgetItem *item = new QTableWidgetItem(QString());
  item->setFlags(item->flags() & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));
  setItem(rowCount() - 1, 0, item);

  if(m_Prop.valid())
  {
    QWidget *w = makeCellWidget(1, OrderedItemExtras::CustomProp);
    QCheckBox *c = qobject_cast<QCheckBox *>(w);
    if(c)
      c->setChecked(m_Prop.defaultValue);
    w->setToolTip(m_Prop.tooltip);

    QWidget *wrapperWidget = new QWidget();
    QHBoxLayout *l = new QHBoxLayout();
    l->setAlignment(Qt::AlignCenter);
    l->addWidget(w);
    l->setContentsMargins(QMargins(0, 0, 0, 0));
    wrapperWidget->setLayout(l);
    setCellWidget(rowCount() - 1, 1, wrapperWidget);
  }

  for(int c = 0; c < m_Extras.size(); c++)
  {
    item = new QTableWidgetItem(QString());
    item->setFlags(item->flags() & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));
    setItem(rowCount() - 1, c + firstExtraColumn(), item);

    setCellWidget(rowCount() - 1, c + firstExtraColumn(),
                  makeCellWidget(c + firstExtraColumn(), m_Extras[c]));
  }
}

QStringList OrderedListEditor::getItems()
{
  QStringList ret;

  int count = rowCount();
  // don't include the last 'new item' entry
  if(allowAddition())
    count--;
  for(int i = 0; i < count; i++)
    ret << item(i, 0)->text();

  return ret;
}

QList<bool> OrderedListEditor::getItemProps()
{
  QList<bool> ret;

  int count = rowCount();
  // don't include the last 'new item' entry
  if(allowAddition())
    count--;
  for(int i = 0; i < count; i++)
  {
    QCheckBox *c = cellWidget(i, 1)->findChild<QCheckBox *>();
    ret << (c && c->isChecked());
  }

  return ret;
}

void OrderedListEditor::cellChanged(int row, int column)
{
  // hack :(. Assume this will only be hit on single UI thread.
  static bool recurse = false;

  if(recurse)
    return;

  recurse = true;

  // if the last row has something added to it, make a new final row
  if(row == rowCount() - 1)
  {
    if(!item(row, column)->data(Qt::DisplayRole).toString().trimmed().isEmpty())
    {
      // enable dragging
      item(row, 0)->setFlags(item(row, 0)->flags() | (Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));
      for(int c = 0; c < m_Extras.size(); c++)
        delete takeItem(row, c + firstExtraColumn());

      addNewItemRow();
    }
  }

  if(row > 0 && column == 0 && item(row, column)->data(Qt::DisplayRole).toString().trimmed().isEmpty())
  {
    removeRow(row);
  }

  recurse = false;
}

void OrderedListEditor::extraClicked(int row, OrderedItemExtras extra)
{
  if(extra == OrderedItemExtras::Delete)
  {
    // don't delete the last 'new item' entry
    if(!allowAddition() || row != rowCount() - 1)
      removeRow(row);
    return;
  }

  QString sel;
  if(extra == OrderedItemExtras::BrowseFolder)
    sel = RDDialog::getExistingDirectory(this, tr("Browse for a folder"));
  else if(extra == OrderedItemExtras::BrowseFile)
    sel = RDDialog::getOpenFileName(this, tr("Browse for a file"));

  if(!sel.isEmpty())
    item(row, 0)->setText(sel);
}

void OrderedListEditor::keyPressEvent(QKeyEvent *event)
{
  if(event->key() == Qt::Key_Delete)
  {
    int row = -1;
    if(selectionModel()->selectedIndexes().count() > 0)
      row = selectionModel()->selectedIndexes()[0].row();

    if(row >= 0)
    {
      // don't delete the last 'new item' entry
      if(!allowAddition() || row != rowCount() - 1)
        removeRow(row);
    }
  }

  RDTableWidget::keyPress(event);
}
