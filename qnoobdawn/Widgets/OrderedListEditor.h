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

#pragma once

#include "Widgets/Extended/RDTableWidget.h"

class QToolButton;

enum class OrderedItemExtras
{
  None = 0x0,
  BrowseFolder = 0x1,
  BrowseFile = 0x2,
  Delete = 0x4,
  CustomProp = 0x8,
};

constexpr inline OrderedItemExtras operator|(OrderedItemExtras a, OrderedItemExtras b)
{
  return OrderedItemExtras(int(a) | int(b));
}

constexpr inline bool operator&(OrderedItemExtras a, OrderedItemExtras b)
{
  return int(a) & int(b);
}

class OrderedListEditor : public RDTableWidget
{
  Q_OBJECT

public:
  struct CustomProp
  {
    QString name, tooltip;
    bool defaultValue;

    bool valid() const { return !name.isEmpty(); }
  };

  explicit OrderedListEditor(const QString &itemName, OrderedItemExtras extras,
                             const CustomProp &prop = {}, QWidget *parent = 0);
  ~OrderedListEditor();

  void setItemsAndProp(const QStringList &strings, const QList<bool> &props);
  void setItems(const QStringList &strings) { setItemsAndProp(strings, {}); }
  QStringList getItems();
  QList<bool> getItemProps();

  bool allowAddition() { return m_allowAddition; }
  void setAllowAddition(bool allow) { m_allowAddition = allow; }

private slots:
  // manual slots
  void cellChanged(int row, int column);
  void extraClicked(int row, OrderedItemExtras extra);

private:
  void keyPressEvent(QKeyEvent *e) override;

  int firstExtraColumn() { return m_Prop.valid() ? 2 : 1; }

  QList<OrderedItemExtras> m_Extras;
  CustomProp m_Prop;

  bool m_allowAddition = true;

  void addNewItemRow();
  QWidget *makeCellWidget(int col, OrderedItemExtras extra);
};
