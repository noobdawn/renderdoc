/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2025-2026 Baldur Karlsson
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

#include <QDialog>
#include "Code/Interface/QRDInterface.h"

namespace Ui
{
class CameraControlsDialog;
}

enum class KeyPressDirection
{
  Forward,
  Back,
  Left,
  Right,
  Up,
  Down,
  Count,
  None = Count,
  NumSettings = Count * 2,
};

constexpr int keySettingIdx(KeyPressDirection dir, bool primary)
{
  return int(dir) * 2 + (primary ? 0 : 1);
}

Qt::Key getDefaultKey(KeyPressDirection dir, bool primary);

constexpr Qt::Key getKeySetting(uint32_t k)
{
  return k != 0 && k < 0x10000000U ? Qt::Key(k) : Qt::Key_unknown;
}

constexpr uint32_t makeKeySetting(Qt::Key k)
{
  return k;
}

constexpr Qt::MouseButton getMouseButtonSetting(uint32_t b)
{
  return b & 0x10000000U ? Qt::MouseButton(b & ~0x10000000U) : Qt::MaxMouseButton;
}

constexpr uint32_t makeMouseButtonSetting(Qt::MouseButton b)
{
  return uint32_t(b) | 0x10000000U;
}

inline QPoint getMouseWheelSetting(uint32_t w)
{
  switch(w)
  {
    case 0x20000000U + 0: return QPoint(0, 1);
    case 0x20000000U + 1: return QPoint(0, -1);
    case 0x20000000U + 2: return QPoint(1, 0);
    case 0x20000000U + 3: return QPoint(-1, 0);
    default: break;
  }
  return QPoint();
}

inline uint32_t makeMouseWheelSetting(QPoint angleDelta)
{
  if(angleDelta.y() > 0)
    return 0x20000000U + 0;
  else if(angleDelta.y() < 0)
    return 0x20000000U + 1;
  else if(angleDelta.x() < 0)
    return 0x20000000U + 2;
  else if(angleDelta.x() > 0)
    return 0x20000000U + 3;
  return 0;
}

constexpr uint32_t makeUnboundSetting()
{
  return 0x40000000U;
}

struct ICaptureContext;
class QDialog;

class CameraControlsDialog : public QDialog
{
  Q_OBJECT
public:
  explicit CameraControlsDialog(ICaptureContext &Ctx, QWidget *parent = 0);
  ~CameraControlsDialog();

private slots:
  void applyUpdatedControls();
  void on_resetAll_clicked();
  void setKey();

private:
  Ui::CameraControlsDialog *ui;
  ICaptureContext &m_Ctx;
  QDialog *m_KeybindDialog;
  uint32_t m_Keybind;

  nbdarray<uint32_t> m_Keys;

  bool eventFilter(QObject *watched, QEvent *event) override;

  QString nameForSetting(uint32_t k);
  QString buttonName(Qt::MouseButton button);
  QString wheelName(QPoint angleDelta);
  void updateDisplayLabels();
};
