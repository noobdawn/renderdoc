/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2025 Baldur Karlsson
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

#include "Extended/RDHeaderView.h"
#include "Extended/RDTreeWidget.h"

struct AnnotationResourceTag
{
  AnnotationResourceTag() = default;

  ResourceId resourceId;
  ResourceType resourceType = ResourceType::Unknown;

  // Buffer-specific fields
  uint64_t bufferOffset = 0;
  uint64_t bufferSize = UINT64_MAX;
  nbdstr bufferFormat;
};

Q_DECLARE_METATYPE(AnnotationResourceTag);

// can be used either as an embedded control in the resource inspector, or as a separate panel for
// monitoring API events
class AnnotationDisplay : public QFrame, public IAnnotationViewer, public ICaptureViewer
{
  Q_OBJECT

public:
  explicit AnnotationDisplay(ICaptureContext &ctx, bool standalone, QWidget *parent = 0);
  ~AnnotationDisplay();

  // IAnnotationViewer
  QWidget *Widget() override { return this; }
  void RevealAnnotation(const nbdstr &keyPath) override;

  // ICaptureViewer
  void OnCaptureLoaded() override;
  void OnCaptureClosed() override;
  void OnSelectedEventChanged(uint32_t eventId) override;
  void OnEventChanged(uint32_t eventId) override {}

  void setAnnotationObject(const SDObject *annotation);

private slots:
  void customContextMenu(QModelIndex index, QMenu *menu);
  void itemClicked(RDTreeWidgetItem *item, int column);

protected:

private:
  ICaptureContext &m_Ctx;
  const SDObject *m_Annotation = NULL;

  RDTreeWidget *m_Tree;
  RDHeaderView *m_Header;
  RDTreeViewExpansionState m_Expansion;

  // if this is a standalone viewer or not
  bool m_Standalone = false;

  // whether the Go column is present
  bool m_HasGoColumn = false;

  QMap<const SDObject *, RDTreeWidgetItem *> m_Items;

  void addStructuredChildren(RDTreeWidgetItem *parent, const SDObject &parentObj);

  bool hasResourceAnnotations(const SDObject &obj);

  // either is a non-empty node, or has at least one non-empty child
  bool shouldBeDisplayed(const SDObject &obj);
};
