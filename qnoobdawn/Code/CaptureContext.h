/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2016-2026 Baldur Karlsson
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

#include <QDebug>
#include <QList>
#include <QMap>
#include <QMessageBox>
#include <QString>
#include <QtWidgets/QWidget>
#include "Interface/QRDInterface.h"
#include "ReplayManager.h"

#if defined(NOOBDAWN_PLATFORM_LINUX)
#include <QX11Info>
#endif

class MainWindow;
class EventBrowser;
class APIInspector;
class AnnotationDisplay;
class PipelineStateViewer;
class BufferViewer;
class TextureViewer;
class CaptureDialog;
class DebugMessageView;
class LogView;
class CommentView;
class PerformanceCounterViewer;
class StatisticsViewer;
class TimelineBar;
class PythonShell;
class ResourceInspector;
class ShaderViewer;
class MiniQtHelper;

class QFileSystemWatcher;

class CaptureContext : public ICaptureContext, IExtensionManager
{
  Q_DECLARE_TR_FUNCTIONS(CaptureContext);

public:
  CaptureContext(PersistantConfig &cfg);
  ~CaptureContext();

  void Begin(QString paramFilename, QString remoteHost, uint32_t remoteIdent, bool temp,
             QString scriptFilename);
  bool isRunning();

  nbdstr TempCaptureFilename(const nbdstr &appname) override;

  //////////////////////////////////////////////////////////////////////////////
  // IExtensionManager

  nbdarray<ExtensionMetadata> GetInstalledExtensions() override;
  bool IsExtensionLoaded(nbdstr name) override;
  nbdstr LoadExtension(nbdstr name) override;

  void RegisterWindowMenu(WindowMenu base, const nbdarray<nbdstr> &submenus,
                          ExtensionCallback callback) override;
  void RegisterPanelMenu(PanelMenu base, const nbdarray<nbdstr> &submenus,
                         ExtensionCallback callback) override;
  void RegisterContextMenu(ContextMenu base, const nbdarray<nbdstr> &submenus,
                           ExtensionCallback callback) override;

  void MenuDisplaying(ContextMenu contextMenu, QMenu *menu,
                      const ExtensionCallbackData &data) override;
  void MenuDisplaying(PanelMenu panelMenu, QMenu *menu, QWidget *extensionButton,
                      const ExtensionCallbackData &data) override;

  IMiniQtHelper &GetMiniQtHelper() override;

  void MessageDialog(const nbdstr &text, const nbdstr &title = "Python Extension Message") override;
  void ErrorDialog(const nbdstr &text, const nbdstr &title = "Python Extension Error") override;
  DialogButton QuestionDialog(const nbdstr &text, const nbdarray<DialogButton> &options,
                              const nbdstr &title = "Python Extension Prompt") override;

  nbdstr OpenFileName(const nbdstr &caption = "Open a file", const nbdstr &dir = nbdstr(),
                      const nbdstr &filter = nbdstr()) override;

  nbdstr OpenDirectoryName(const nbdstr &caption = "Open a directory",
                           const nbdstr &dir = nbdstr()) override;

  nbdstr SaveFileName(const nbdstr &caption = "Save a file", const nbdstr &dir = nbdstr(),
                      const nbdstr &filter = nbdstr()) override;

  //////////////////////////////////////////////////////////////////////////////
  // Control functions

  void LoadCapture(const nbdstr &captureFile, const ReplayOptions &opts, const nbdstr &origFilename,
                   bool temporary, bool local) override;
  bool SaveCaptureTo(const nbdstr &captureFile) override;
  void RecompressCapture() override;
  void CloseCapture() override;
  bool ImportCapture(const CaptureFileFormat &fmt, const nbdstr &importfile,
                     const nbdstr &nbdfile) override;
  void ExportCapture(const CaptureFileFormat &fmt, const nbdstr &exportfile) override;
  void SetEventID(const nbdarray<ICaptureViewer *> &exclude, uint32_t selectedEventID,
                  uint32_t eventId, bool force = false) override;
  void SetRemoteHost(int hostIndex);
  void RefreshStatus() override { SetEventID({}, m_SelectedEventID, m_EventID, true); }
  bool IsResourceReplaced(ResourceId id) override;
  ResourceId GetResourceReplacement(ResourceId id) override;
  void RegisterReplacement(ResourceId from, ResourceId to) override;
  void UnregisterReplacement(ResourceId id) override;
  void RefreshUIStatus(const nbdarray<ICaptureViewer *> &exclude, bool updateSelectedEvent,
                       bool updateEvent);

  void AddCaptureViewer(ICaptureViewer *f) override
  {
    m_CaptureViewers.push_back(f);

    if(IsCaptureLoaded())
    {
      f->OnCaptureLoaded();
      f->OnEventChanged(CurEvent());
    }
  }

  void RemoveCaptureViewer(ICaptureViewer *f) override { m_CaptureViewers.removeAll(f); }
  //////////////////////////////////////////////////////////////////////////////
  // Accessors

  IReplayManager &Replay() override { return m_Replay; }
  IExtensionManager &Extensions() override { return *this; }
  bool IsCaptureLoaded() override { return m_CaptureLoaded; }
  bool IsCaptureLocal() override { return m_CaptureLocal; }
  bool IsCaptureTemporary() override { return m_CaptureTemporary; }
  bool IsCaptureLoading() override { return m_LoadInProgress; }
  ResultDetails GetFatalError() override { return m_Replay.GetFatalError(); }
  nbdstr GetCaptureFilename() override { return m_CaptureFile; }
  CaptureModifications GetCaptureModifications() override { return m_CaptureMods; }
  const FrameDescription &FrameInfo() override { return m_FrameInfo; }
  const APIProperties &APIProps() override { return m_APIProps; }
  nbdarray<ShaderEncoding> CustomShaderEncodings() override { return m_CustomEncodings; }
  nbdarray<ShaderSourcePrefix> CustomShaderSourcePrefixes() override { return m_CustomPrefixes; }
  nbdarray<ShaderEncoding> TargetShaderEncodings() override { return m_TargetEncodings; }
  uint32_t CurSelectedEvent() override { return m_SelectedEventID; }
  uint32_t CurEvent() override { return m_EventID; }
  const ActionDescription *CurSelectedAction() override { return GetAction(CurSelectedEvent()); }
  const ActionDescription *CurAction() override { return GetAction(CurEvent()); }
  const ActionDescription *GetFirstAction() override { return m_FirstAction; };
  const ActionDescription *GetLastAction() override { return m_LastAction; };
  void ClearReplayCache() override;
  bool OpenRGPProfile(const nbdstr &filename) override;
  IRGPInterop *GetRGPInterop() override { return m_RGP; }
  const nbdarray<ActionDescription> &CurRootActions() override { return *m_Actions; }
  const ResourceDescription *GetResource(ResourceId id) const override { return m_Resources[id]; }
  const nbdarray<ResourceDescription> &GetResources() override { return m_ResourceList; }
  nbdstr GetResourceName(ResourceId id) const override;
  nbdstr GetResourceNameUnsuffixed(ResourceId id) const override;
  bool IsAutogeneratedName(ResourceId id) override;
  bool HasResourceCustomName(ResourceId id) override;
  void SetResourceCustomName(ResourceId id, const nbdstr &name) override;
  int32_t ResourceNameCacheID() const override { return m_CustomNameCachedID; }
  TextureDescription *GetTexture(ResourceId id) override { return m_Textures[id]; }
  const nbdarray<TextureDescription> &GetTextures() override { return m_TextureList; }
  BufferDescription *GetBuffer(ResourceId id) override { return m_Buffers[id]; }
  DescriptorStoreDescription *GetDescriptorStore(ResourceId id) override
  {
    return m_DescriptorStores[id];
  }
  const nbdarray<BufferDescription> &GetBuffers() const override { return m_BufferList; }
  const ActionDescription *GetAction(uint32_t eventId) override
  {
    return GetAction(*m_Actions, eventId);
  }
  const SDFile &GetStructuredFile() override { return *m_StructuredFile; }
  WindowingSystem CurWindowingSystem() override { return m_CurWinSystem; }
  WindowingData CreateWindowingData(QWidget *window) override;

  const nbdarray<DebugMessage> &DebugMessages() override { return m_DebugMessages; }
  int32_t UnreadMessageCount() override { return m_UnreadMessageCount; }
  void MarkMessagesRead() override { m_UnreadMessageCount = 0; }
  void AddMessages(const nbdarray<DebugMessage> &msgs) override;
  void ClearMessages() override;

  void ConnectToRemoteServer(RemoteHost host) override;

  nbdstr GetNotes(const nbdstr &key) override { return m_Notes[key]; }
  void SetNotes(const nbdstr &key, const nbdstr &contents) override;
  nbdarray<EventBookmark> GetBookmarks() override { return m_Bookmarks; }
  void SetBookmark(const EventBookmark &mark) override;
  void RemoveBookmark(uint32_t EID) override;
  void EmbedDependentFiles() override;
  void RemoveDependentFiles() override;

  void DelayedCallback(uint32_t milliseconds, std::function<void()> callback) override;

  IMainWindow *GetMainWindow() override;
  IEventBrowser *GetEventBrowser() override;
  IAPIInspector *GetAPIInspector() override;
  IAnnotationViewer *GetAnnotationViewer() override;
  ITextureViewer *GetTextureViewer() override;
  IBufferViewer *GetMeshPreview() override;
  IPipelineStateViewer *GetPipelineViewer() override;
  ICaptureDialog *GetCaptureDialog() override;
  IDebugMessageView *GetDebugMessageView() override;
  IDiagnosticLogView *GetDiagnosticLogView() override;
  ICommentView *GetCommentView() override;
  IPerformanceCounterViewer *GetPerformanceCounterViewer() override;
  IStatisticsViewer *GetStatisticsViewer() override;
  ITimelineBar *GetTimelineBar() override;
  IPythonShell *GetPythonShell() override;
  IResourceInspector *GetResourceInspector() override;

  bool HasEventBrowser() override { return m_EventBrowser != NULL; }
  bool HasAPIInspector() override { return m_APIInspector != NULL; }
  bool HasAnnotationViewer() override { return m_AnnotationViewer != NULL; }
  bool HasTextureViewer() override { return m_TextureViewer != NULL; }
  bool HasPipelineViewer() override { return m_PipelineViewer != NULL; }
  bool HasMeshPreview() override { return m_MeshPreview != NULL; }
  bool HasCaptureDialog() override { return m_CaptureDialog != NULL; }
  bool HasDebugMessageView() override { return m_DebugMessageView != NULL; }
  bool HasDiagnosticLogView() override { return m_DiagnosticLogView != NULL; }
  bool HasCommentView() override { return m_CommentView != NULL; }
  bool HasPerformanceCounterViewer() override { return m_PerformanceCounterViewer != NULL; }
  bool HasStatisticsViewer() override { return m_StatisticsViewer != NULL; }
  bool HasTimelineBar() override { return m_TimelineBar != NULL; }
  bool HasPythonShell() override { return m_PythonShell != NULL; }
  bool HasResourceInspector() override { return m_ResourceInspector != NULL; }
  void ShowEventBrowser() override;
  void ShowAPIInspector() override;
  void ShowAnnotationViewer() override;
  void ShowTextureViewer() override;
  void ShowMeshPreview() override;
  void ShowPipelineViewer() override;
  void ShowCaptureDialog() override;
  void ShowDebugMessageView() override;
  void ShowDiagnosticLogView() override;
  void ShowCommentView() override;
  void ShowPerformanceCounterViewer() override;
  void ShowStatisticsViewer() override;
  void ShowTimelineBar() override;
  void ShowPythonShell() override;
  void ShowResourceInspector() override;

  IShaderViewer *EditShader(ResourceId id, ShaderStage stage, const nbdstr &entryPoint,
                            const nbdstrpairs &files, KnownShaderTool knownTool,
                            ShaderEncoding shaderEncoding, ShaderCompileFlags flags,
                            IShaderViewer::SaveCallback saveCallback,
                            IShaderViewer::RevertCallback revertCallback) override;

  void ApplyShaderEdit(IShaderViewer *viewer, ResourceId id, ShaderStage stage,
                       ShaderEncoding shaderEncoding, ShaderCompileFlags flags,
                       const nbdstr &entryFunc, const bytebuf &shaderBytes);
  void RevertShaderEdit(IShaderViewer *viewer, ResourceId id);

  IShaderViewer *DebugShader(const ShaderReflection *shader, ResourceId pipeline,
                             ShaderDebugTrace *trace, const nbdstr &debugContext) override;

  IShaderViewer *ViewShader(const ShaderReflection *shader, ResourceId pipeline) override;

  IShaderMessageViewer *ViewShaderMessages(ShaderStageMask stages) override;

  IDescriptorViewer *ViewDescriptorStore(ResourceId id) override;
  IDescriptorViewer *ViewDescriptors(const nbdarray<Descriptor> &descriptors,
                                     const nbdarray<SamplerDescriptor> &samplerDescriptors) override;
  IBufferViewer *ViewBuffer(uint64_t byteOffset, uint64_t byteSize, ResourceId id,
                            const nbdstr &format = "") override;
  IBufferViewer *ViewTextureAsBuffer(ResourceId id, const Subresource &sub,
                                     const nbdstr &format = "") override;

  IBufferViewer *ViewConstantBuffer(ShaderStage stage, uint32_t slot, uint32_t idx) override;
  IPixelHistoryView *ViewPixelHistory(ResourceId texID, uint32_t x, uint32_t y, uint32_t view,
                                      const TextureDisplay &display) override;

  QWidget *CreateBuiltinWindow(const nbdstr &objectName) override;
  void BuiltinWindowClosed(QWidget *window) override;

  void RaiseDockWindow(QWidget *dockWindow) override;
  void AddDockWindow(QWidget *newWindow, DockReference ref, QWidget *refWindow,
                     float percentage = 0.5f) override;

  const D3D11Pipe::State *CurD3D11PipelineState() override { return m_CurD3D11PipelineState; }
  const D3D12Pipe::State *CurD3D12PipelineState() override { return m_CurD3D12PipelineState; }
  const GLPipe::State *CurGLPipelineState() override { return m_CurGLPipelineState; }
  const VKPipe::State *CurVulkanPipelineState() override { return m_CurVulkanPipelineState; }
  const PipeState &CurPipelineState() override { return *m_CurPipelineState; }
  PersistantConfig &Config() override { return m_Config; }
private:
  ReplayManager m_Replay;

  const D3D11Pipe::State *m_CurD3D11PipelineState;
  const D3D12Pipe::State *m_CurD3D12PipelineState;
  const GLPipe::State *m_CurGLPipelineState;
  const VKPipe::State *m_CurVulkanPipelineState;
  const PipeState *m_CurPipelineState;
  PipeState m_DummyPipelineState;

  PersistantConfig &m_Config;

  QVector<ICaptureViewer *> m_CaptureViewers;

  bool m_CaptureLoaded = false, m_LoadInProgress = false, m_CaptureLocal = false,
       m_CaptureTemporary = false;
  QString m_CaptureFile;
  QString m_RemoteFile;
  CaptureModifications m_CaptureMods = CaptureModifications::NoModifications;

  nbdarray<DebugMessage> m_DebugMessages;
  int m_UnreadMessageCount = 0;

  void SetModification(CaptureModifications mod);

  void SaveChanges();

  bool SaveRenames();
  void LoadRenames(const QString &data);

  bool SaveBookmarks();
  void LoadBookmarks(const QString &data);

  bool SaveNotes();
  void LoadNotes(const QString &data);

  bool SaveEdits();
  void LoadEdits(const QString &data);

  void CacheResources();
  nbdstr GetResourceNameUnsuffixed(const ResourceDescription *desc) const;

  float m_LoadProgress = 0.0f;
  float m_PostloadProgress = 0.0f;
  float UpdateLoadProgress();

  void LoadCaptureThreaded(const QString &captureFile, const ReplayOptions &opts,
                           const QString &origFilename, bool temporary, bool local);

  void AddSortedMenuItem(QMenu *menu, bool rootMenu, const nbdarray<nbdstr> &items,
                         std::function<void()> callback);
  void CleanMenu(QAction *action);

  uint32_t m_SelectedEventID = 0;
  uint32_t m_EventID = 0;

  const ActionDescription *GetAction(const nbdarray<ActionDescription> &actions, uint32_t eventId)
  {
    for(const ActionDescription &a : actions)
    {
      if(!a.children.empty())
      {
        const ActionDescription *action = GetAction(a.children, eventId);
        if(action != NULL)
          return action;
      }

      if(a.eventId == eventId)
        return &a;
    }

    return NULL;
  }

  void setupDockWindow(QWidget *shad, bool hide);
  const nbdarray<ActionDescription> *m_Actions;
  nbdarray<ActionDescription> m_EmptyActions;

  nbdarray<ShaderEncoding> m_CustomEncodings, m_TargetEncodings;
  nbdarray<ShaderSourcePrefix> m_CustomPrefixes;
  APIProperties m_APIProps;
  FrameDescription m_FrameInfo;
  const ActionDescription *m_FirstAction = NULL;
  const ActionDescription *m_LastAction = NULL;

  IRGPInterop *m_RGP = NULL;

  QMap<ResourceId, TextureDescription *> m_Textures;
  nbdarray<TextureDescription> m_TextureList;
  QMap<ResourceId, BufferDescription *> m_Buffers;
  nbdarray<BufferDescription> m_BufferList;
  QMap<ResourceId, DescriptorStoreDescription *> m_DescriptorStores;
  nbdarray<DescriptorStoreDescription> m_DescriptorStoreList;
  QMap<ResourceId, ResourceDescription *> m_Resources;
  nbdarray<ResourceDescription> m_ResourceList;

  QList<EventBookmark> m_Bookmarks;

  QMap<QString, QString> m_Notes;

  QMap<ResourceId, QString> m_CustomNames;
  int m_CustomNameCachedID = 1;

  // map orig replaced -> edited replacement ID
  QMap<ResourceId, ResourceId> m_OrigToReplacedResources;
  // reverse map, edited ID -> orig
  QMap<ResourceId, ResourceId> m_ReplacedToOrigResources;

  const SDFile *m_StructuredFile = NULL;
  SDFile m_DummySDFile;

  nbdarray<WindowingSystem> m_WinSystems;

  WindowingSystem m_CurWinSystem = WindowingSystem::Unknown;

#if defined(NOOBDAWN_PLATFORM_LINUX)
  wl_display *m_WaylandDisplay = NULL;
  xcb_connection_t *m_XCBConnection = NULL;
  Display *m_X11Display = NULL;
#endif

  QIcon *m_Icon = NULL;

  QList<QObject *> m_PendingExtensionObjects;
  QMap<nbdstr, QList<QObject *>> m_ExtensionObjects;

  QList<QPointer<RegisteredMenuItem>> m_RegisteredMenuItems;

  QList<ShaderViewer *> m_ShaderEditors;

  MiniQtHelper *m_QtHelper = NULL;

  QFileSystemWatcher *m_Watcher = NULL;

  // Windows
  MainWindow *m_MainWindow = NULL;
  EventBrowser *m_EventBrowser = NULL;
  APIInspector *m_APIInspector = NULL;
  AnnotationDisplay *m_AnnotationViewer = NULL;
  TextureViewer *m_TextureViewer = NULL;
  BufferViewer *m_MeshPreview = NULL;
  PipelineStateViewer *m_PipelineViewer = NULL;
  CaptureDialog *m_CaptureDialog = NULL;
  DebugMessageView *m_DebugMessageView = NULL;
  LogView *m_DiagnosticLogView = NULL;
  CommentView *m_CommentView = NULL;
  PerformanceCounterViewer *m_PerformanceCounterViewer = NULL;
  StatisticsViewer *m_StatisticsViewer = NULL;
  TimelineBar *m_TimelineBar = NULL;
  PythonShell *m_PythonShell = NULL;
  ResourceInspector *m_ResourceInspector = NULL;
};
