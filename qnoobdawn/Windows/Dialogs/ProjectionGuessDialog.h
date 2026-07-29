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

struct ProjectionGuessParameters
{
  bool orthographic = false;
  float fov = 90.0f;
  float aspect = 0.0f;
  float nearPlane = 0.0f;
  float farPlane = 0.0f;
};

namespace Ui
{
class ProjectionGuessDialog;
}

struct ICaptureContext;

class ProjectionGuessDialog : public QDialog
{
  Q_OBJECT
public:
  explicit ProjectionGuessDialog(ICaptureContext &Ctx, const ProjectionGuessParameters &params,
                                 QWidget *parent = 0);
  const ProjectionGuessParameters &getParameters() { return m_Params; }
  ~ProjectionGuessDialog();

public slots:
  void on_infiniteFar_toggled();
  void on_matrixType_currentIndexChanged(int idx);

private slots:
  void validateParameters();

private:
  Ui::ProjectionGuessDialog *ui;
  ICaptureContext &m_Ctx;
  ProjectionGuessParameters m_Params;
};
