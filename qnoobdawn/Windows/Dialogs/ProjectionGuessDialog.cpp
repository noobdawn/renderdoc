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

#include "ProjectionGuessDialog.h"
#include <float.h>
#include "Code/QRDUtils.h"
#include "ui_ProjectionGuessDialog.h"

ProjectionGuessDialog::ProjectionGuessDialog(ICaptureContext &Ctx,
                                             const ProjectionGuessParameters &params, QWidget *parent)
    : QDialog(parent), m_Ctx(Ctx), ui(new Ui::ProjectionGuessDialog)
{
  ui->setupUi(this);
  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

  m_Params = params;

  if(m_Params.orthographic)
    ui->matrixType->setCurrentIndex(1);
  else
    ui->matrixType->setCurrentIndex(0);

  ui->fov->setValue(m_Params.fov);
  if(m_Params.aspect <= 0.0)
    ui->aspectRatio->setText(QString());
  else
    ui->aspectRatio->setText(QFormatStr("%1").arg(m_Params.aspect));

  if(m_Params.nearPlane <= 0.0)
    ui->nearPlane->setText(QString());
  else
    ui->nearPlane->setText(QFormatStr("%1").arg(m_Params.nearPlane));

  if(m_Params.farPlane == FLT_MAX)
    ui->infiniteFar->setChecked(true);
  else if(m_Params.farPlane <= 0.0)
    ui->farPlane->setText(QString());
  else
    ui->farPlane->setText(QFormatStr("%1").arg(m_Params.farPlane));

  connect(ui->buttonBox, &QDialogButtonBox::accepted, this,
          &ProjectionGuessDialog::validateParameters);
  connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void ProjectionGuessDialog::on_infiniteFar_toggled()
{
  if(ui->infiniteFar->isChecked())
  {
    ui->farPlane->setEnabled(false);
    ui->farPlane->setText(tr("Infinite - Reverse Z"));
  }
  else
  {
    ui->farPlane->setEnabled(true);
    ui->farPlane->setText(QString());
  }
}

void ProjectionGuessDialog::on_matrixType_currentIndexChanged(int idx)
{
  const bool ortho = (idx == 1);

  // not used for orthographic projection
  ui->aspectRatio->setEnabled(!ortho);
  ui->fov->setEnabled(!ortho);
}

void ProjectionGuessDialog::validateParameters()
{
  QString str;
  bool ok = false;

  m_Params.orthographic = (ui->matrixType->currentIndex() == 1);

  if(m_Params.orthographic)
  {
    m_Params.aspect = 0.0f;
    m_Params.fov = 90.0f;
  }
  else
  {
    str = ui->aspectRatio->text();
    if(str.isEmpty())
    {
      m_Params.aspect = 0.0f;
    }
    else
    {
      m_Params.aspect = str.toFloat(&ok);
      if(!ok)
      {
        RDDialog::critical(this, tr("Aspect ratio error"),
                           tr("Unrecognised aspect ratio %1, expected decimal number.").arg(str));
        return;
      }
    }

    m_Params.fov = (float)ui->fov->value();
  }

  str = ui->nearPlane->text();
  if(str.isEmpty())
  {
    m_Params.nearPlane = 0.0f;
  }
  else
  {
    m_Params.nearPlane = str.toFloat(&ok);
    if(!ok)
    {
      RDDialog::critical(
          this, tr("Near plane error"),
          tr("Unrecognised near plane %1, expected decimal number if specified.").arg(str));
      return;
    }
  }

  if(ui->infiniteFar->isChecked())
  {
    m_Params.farPlane = FLT_MAX;
  }
  else
  {
    str = ui->farPlane->text();
    if(str.isEmpty())
    {
      m_Params.farPlane = 0.0f;
    }
    else
    {
      m_Params.farPlane = str.toFloat(&ok);
      if(!ok)
      {
        RDDialog::critical(
            this, tr("Far plane error"),
            tr("Unrecognised far plane %1, expected decimal number if specified.").arg(str));
        return;
      }
    }
  }

  if(m_Params.nearPlane > 0 && m_Params.farPlane > 0 && m_Params.nearPlane >= m_Params.farPlane)
  {
    RDDialog::critical(this, tr("Invalid near & far plane"),
                       tr("Near plane %1 must be less than far plane %2.")
                           .arg(m_Params.nearPlane)
                           .arg(m_Params.farPlane));
    return;
  }

  accept();
}

ProjectionGuessDialog::~ProjectionGuessDialog()
{
  delete ui;
}
