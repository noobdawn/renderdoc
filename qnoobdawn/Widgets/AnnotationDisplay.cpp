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

#include "AnnotationDisplay.h"
#include <QAction>
#include <QCollator>
#include <QHeaderView>
#include <QMenu>
#include <QVBoxLayout>
#include "Code/Interface/QRDInterface.h"
#include "Code/QRDUtils.h"
#include "Code/Resources.h"
#include "Extended/RDHeaderView.h"

const int HoverRole = Qt::UserRole;

AnnotationDisplay::AnnotationDisplay(ICaptureContext &ctx, bool standalone, QWidget *parent)
    : QFrame(parent), m_Ctx(ctx), m_Standalone(standalone)
{
  m_Tree = new RDTreeWidget(this);

  m_Header = new RDHeaderView(Qt::Horizontal, this);
  m_Tree->setHeader(m_Header);

  m_Tree->setColumns({lit("Key"), tr("Value")});
  m_Header->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_Header->setSectionResizeMode(1, QHeaderView::Stretch);
  m_Tree->setFont(Formatter::PreferredFont());

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setSpacing(0);
  layout->setMargin(m_Standalone ? 3 : 0);

  layout->addWidget(m_Tree);

  if(!m_Standalone)
  {
    setFrameStyle(QFrame::NoFrame);
    m_Tree->setFrameStyle(QFrame::NoFrame);
  }

  setWindowTitle(tr("Annotation Viewer"));

  QObject::connect(m_Tree, &RDTreeWidget::customContextMenu, this,
                   &AnnotationDisplay::customContextMenu);
  QObject::connect(m_Tree, &RDTreeWidget::itemClicked, this, &AnnotationDisplay::itemClicked);
  // treat a double click the same as a single click on the go column, if we have one
  QObject::connect(m_Tree, &RDTreeWidget::itemDoubleClicked,
                   [this](RDTreeWidgetItem *item, int column) {
                     if(m_HasGoColumn)
                       itemClicked(item, 2);
                   });

  m_Tree->setHoverIconColumn(2, Icons::action(), Icons::action_hover());
  m_Tree->setHoverRole(HoverRole);

  if(m_Standalone)
    m_Ctx.AddCaptureViewer(this);
}

AnnotationDisplay::~AnnotationDisplay()
{
  if(m_Standalone)
    m_Ctx.RemoveCaptureViewer(this);
}

void AnnotationDisplay::RevealAnnotation(const nbdstr &keyPath)
{
  if(m_Annotation)
  {
    const SDObject *obj = m_Annotation->FindChildByKeyPath(keyPath);

    RDTreeWidgetItem *item = m_Items[obj];
    if(item)
    {
      m_Tree->setSelectedItem(item);
      m_Tree->scrollToItem(item);
    }
  }
}

void AnnotationDisplay::OnCaptureLoaded()
{
  APIEvent ev = m_Ctx.GetEventBrowser()->GetAPIEventForEID(m_Ctx.CurSelectedEvent());

  setAnnotationObject(ev.annotations);
}

void AnnotationDisplay::OnCaptureClosed()
{
  setAnnotationObject(NULL);
}

void AnnotationDisplay::OnSelectedEventChanged(uint32_t eventId)
{
  APIEvent ev = m_Ctx.GetEventBrowser()->GetAPIEventForEID(eventId);

  setAnnotationObject(ev.annotations);
}

bool AnnotationDisplay::shouldBeDisplayed(const SDObject &obj)
{
  if(obj.type.flags & SDTypeFlags::Hidden)
    return false;

  if(obj.name.beginsWith("__"))
    return false;

  if(obj.type.basetype == SDBasic::Struct)
  {
    for(const SDObject *child_obj : obj)
    {
      if(shouldBeDisplayed(*child_obj))
        return true;
    }
    return false;
  }

  return true;
}

void AnnotationDisplay::addStructuredChildren(RDTreeWidgetItem *parent, const SDObject &parentObj)
{
  QCollator collator;
  collator.setNumericMode(true);
  collator.setCaseSensitivity(Qt::CaseInsensitive);

  nbdarray<const SDObject *> children;
  children.reserve(parentObj.NumChildren());
  for(const SDObject *obj : parentObj)
    children.push_back(obj);

  if(parentObj.type.basetype != SDBasic::Array)
    std::sort(children.begin(), children.end(), [&collator](const SDObject *a, const SDObject *b) {
      return collator.compare(QString(a->name), QString(b->name)) < 0;
    });

  for(const SDObject *obj : children)
  {
    if(!shouldBeDisplayed(*obj))
      continue;

    QVariant name;

    if(parentObj.type.basetype == SDBasic::Array)
      name = QFormatStr("[%1]").arg(parent->childCount());
    else
      name = obj->name;

    RDTreeWidgetItem *item;
    if(m_HasGoColumn)
      item = new RDTreeWidgetItem({name, QString(), QString()});
    else
      item = new RDTreeWidgetItem({name, QString()});

    m_Items[obj] = item;

    // Check if this is a viewable resource (buffer, texture, or shader)
    if(obj->type.basetype == SDBasic::Resource)
    {
      ResourceId id = obj->data.basic.id;
      const ResourceDescription *resDesc = m_Ctx.GetResource(id);

      if(resDesc && (resDesc->type == ResourceType::Buffer ||
                     resDesc->type == ResourceType::Texture || resDesc->type == ResourceType::Shader))
      {
        AnnotationResourceTag tag;
        tag.resourceId = id;
        tag.resourceType = resDesc->type;

        if(resDesc->type == ResourceType::Buffer)
        {
          // Look for special child annotations for buffer viewer parameters
          const SDObject *offsetChild = obj->FindChildByKeyPath("__offset");
          const SDObject *sizeChild = obj->FindChildByKeyPath("__size");
          const SDObject *formatChild = obj->FindChildByKeyPath("__rd_format");

          if(offsetChild && (offsetChild->type.basetype == SDBasic::UnsignedInteger ||
                             offsetChild->type.basetype == SDBasic::SignedInteger))
            tag.bufferOffset = offsetChild->data.basic.u;

          if(sizeChild && (sizeChild->type.basetype == SDBasic::UnsignedInteger ||
                           sizeChild->type.basetype == SDBasic::SignedInteger))
            tag.bufferSize = sizeChild->data.basic.u;

          if(formatChild && formatChild->type.basetype == SDBasic::String)
            tag.bufferFormat = formatChild->data.str;
        }

        item->setTag(QVariant::fromValue(tag));

        item->setData(2, HoverRole, true);

        if(m_HasGoColumn)
          item->setIcon(2, Icons::action());
      }
    }

    if(obj->type.basetype == SDBasic::Chunk || obj->type.basetype == SDBasic::Struct ||
       obj->type.basetype == SDBasic::Array)
      addStructuredChildren(item, *obj);
    else if(obj->type.basetype == SDBasic::String)
      item->setText(1, QString(obj->data.str));
    else
      item->setText(1, SDObject2Variant(obj, false));

    parent->addChild(item);
  }
}

bool AnnotationDisplay::hasResourceAnnotations(const SDObject &obj)
{
  if(obj.type.basetype == SDBasic::Resource)
  {
    const ResourceDescription *resDesc = m_Ctx.GetResource(obj.data.basic.id);
    if(resDesc && (resDesc->type == ResourceType::Buffer ||
                   resDesc->type == ResourceType::Texture || resDesc->type == ResourceType::Shader))
      return true;
  }

  for(const SDObject *child : obj)
  {
    if(child->type.flags & SDTypeFlags::Hidden)
      continue;
    if(child->name.beginsWith("__"))
      continue;
    if(hasResourceAnnotations(*child))
      return true;
  }

  return false;
}

void AnnotationDisplay::setAnnotationObject(const SDObject *annotation)
{
  m_Tree->updateExpansion(m_Expansion, 0);

  m_Annotation = annotation;

  m_Items.clear();
  m_Tree->invisibleRootItem()->clear();

  // Check if we have any buffer annotations to determine columns
  m_HasGoColumn = m_Annotation && hasResourceAnnotations(*m_Annotation);

  if(m_HasGoColumn)
  {
    m_Tree->setColumns({lit("Key"), tr("Value"), tr("Go")});
    m_Header->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_Header->setSectionResizeMode(1, QHeaderView::Stretch);
    m_Header->setSectionResizeMode(2, QHeaderView::Fixed);
    m_Header->resizeSection(2, 16);
  }
  else
  {
    m_Tree->setColumns({lit("Key"), tr("Value")});
    m_Header->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_Header->setSectionResizeMode(1, QHeaderView::Stretch);
  }

  if(m_Annotation)
  {
    m_Tree->beginUpdate();
    addStructuredChildren(m_Tree->invisibleRootItem(), *m_Annotation);
    m_Tree->endUpdate();
  }

  m_Tree->applyExpansion(m_Expansion, 0);
}

void AnnotationDisplay::customContextMenu(QModelIndex index, QMenu *menu)
{
  RDTreeWidgetItem *item = m_Tree->itemForIndex(index);

  const SDObject *obj = m_Items.key(item, NULL);
  if(!obj)
    return;

  nbdstr path;
  // don't include the root node, it's not part of the path, so only iterate over nodes that have
  // parents themselves
  while(obj && obj->GetParent())
  {
    if(path.empty())
      path = obj->name;
    else
      path = obj->name + nbdstr(".") + path;
    obj = obj->GetParent();
  }

  if(path.empty())
    return;

  QAction *sep = menu->insertSeparator(menu->actions()[0]);

  QAction *showInEventBrowser = new QAction(tr("&Highlight in Event Browser"), menu);

  QObject::connect(showInEventBrowser, &QAction::triggered,
                   [this, path]() { m_Ctx.GetEventBrowser()->SetHighlightedAnnotation(path); });

  menu->insertAction(sep, showInEventBrowser);
}

void AnnotationDisplay::itemClicked(RDTreeWidgetItem *item, int column)
{
  if(!m_HasGoColumn || column != 2)
    return;

  QVariant tag = item->tag();

  if(!tag.canConvert<AnnotationResourceTag>())
    return;

  AnnotationResourceTag resTag = tag.value<AnnotationResourceTag>();

  if(resTag.resourceType == ResourceType::Buffer)
  {
    IBufferViewer *viewer = m_Ctx.ViewBuffer(resTag.bufferOffset, resTag.bufferSize,
                                             resTag.resourceId, resTag.bufferFormat);
    m_Ctx.AddDockWindow(viewer->Widget(), DockReference::MainToolArea, NULL);
  }
  else if(resTag.resourceType == ResourceType::Texture)
  {
    if(!m_Ctx.HasTextureViewer())
      m_Ctx.ShowTextureViewer();
    ITextureViewer *viewer = m_Ctx.GetTextureViewer();
    viewer->ViewTexture(resTag.resourceId, CompType::Typeless, true);
  }
  else if(resTag.resourceType == ResourceType::Shader)
  {
    ResourceId id = resTag.resourceId;
    ICaptureContext *ctx = &m_Ctx;
    m_Ctx.Replay().AsyncInvoke([this, ctx, id](IReplayController *r) {
      nbdarray<ShaderEntryPoint> entries = r->GetShaderEntryPoints(id);
      if(entries.isEmpty())
        return;

      const ShaderReflection *refl = r->GetShader(ResourceId(), id, entries[0]);
      if(!refl)
        return;

      GUIInvoke::call(this, [ctx, refl] {
        IShaderViewer *viewer = ctx->ViewShader(refl, ResourceId());
        ctx->AddDockWindow(viewer->Widget(), DockReference::MainToolArea, NULL);
      });
    });
  }
}
