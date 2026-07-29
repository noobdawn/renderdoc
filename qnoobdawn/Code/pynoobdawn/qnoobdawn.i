
%module(docstring="This is the API to QNoobDawn's high-level UI panels and functionality.") qnoobdawn

%feature("autodoc", "0");

// use documentation for docstrings
#define DOCUMENT(text) %feature("docstring") text
#define DOCUMENT2(text1, text2) %feature("docstring") text1 text2
#define DOCUMENT3(text1, text2, text3) %feature("docstring") text1 text2 text3
#define DOCUMENT4(text1, text2, text3, text4) %feature("docstring") text1 text2 text3 text4

%header %{
#include "3rdparty/pythoncapi_compat.h"
%}

%begin %{

#include <Python.h>

#if PY_VERSION_HEX >= 0x030d0000
inline PyObject *PyWeakref_GetObject_emu(PyObject *ref)
{
  PyObject *ret = NULL;
  if(PyWeakref_GetRef(ref, &ret) > 0)
    Py_DECREF(ret);
  return ret;
}
#undef PyWeakref_GET_OBJECT
#define PyWeakref_GET_OBJECT(x) PyWeakref_GetObject_emu(x)
#endif

%}

%begin %{
#undef slots

#ifndef SWIG_GENERATED
#define SWIG_GENERATED
#endif

// we want visual assist to ignore this file, because it's a *lot* of generated code and has no
// useful results. This macro does nothing on normal builds, but is defined to _asm { in va_stdafx.h
#define VA_IGNORE_REST_OF_FILE
VA_IGNORE_REST_OF_FILE
%}

%{
  #include "datetime.h"
%}
%init %{
  PyDateTime_IMPORT;
%}

%{
  #include "Code/Interface/QRDInterface.h"
%}

%include "pyconversion.i"

// import the noobdawn interface that we depend on
%import "noobdawn.i"

TEMPLATE_ARRAY_DECLARE(nbdarray);
TEMPLATE_FIXEDARRAY_DECLARE(nbdfixedarray);

// pass QWidget objects to PySide
%{
  class QWidget;

  extern "C" QWidget *QWidgetFromPy(PyObject *widget);
  extern "C" PyObject *QWidgetToPy(QWidget *widget);

  DECLARE_STRINGISE_TYPE(QWidget);
%}

%typemap(in) QWidget * {
  if(Py_IsNone($input))
    $1 = NULL;
  else
    $1 = QWidgetFromPy($input);
  if($input && !Py_IsNone($input) && !$1)
  {
    SWIG_exception_fail(SWIG_TypeError, "in method '$symname' QWidget expected for argument $argnum of type '$1_basetype'");
  }
}

%typemap(out) QWidget * {
  $result = QWidgetToPy($1);
}

// create a wrapper for passing python ICaptureViewer interface implementations to C++

%wrapper %{
  struct PythonCaptureViewer : public ICaptureViewer
  {
    PythonCaptureViewer(PyObject *s);

    virtual ~PythonCaptureViewer()
    {
      Py_DECREF(self);
    }
    void OnCaptureLoaded() override { if(m_OnCaptureLoaded) m_OnCaptureLoaded(); }
    void OnCaptureClosed() override { if(m_OnCaptureClosed) m_OnCaptureClosed(); }
    void OnSelectedEventChanged(uint32_t eventId) override { if(m_OnSelectedEventChanged) m_OnSelectedEventChanged(eventId); }
    void OnEventChanged(uint32_t eventId) override { if(m_OnEventChanged) m_OnEventChanged(eventId); }

  private:
    PyObject *self;

    std::function<void()> m_OnCaptureLoaded, m_OnCaptureClosed;
    std::function<void(uint32_t)> m_OnSelectedEventChanged, m_OnEventChanged;
  };

  static int capviewer_init(PyObject *self, PyObject *args) {
    PyObject *resultobj = 0;
    ICaptureViewer *result = 0;

    result = new PythonCaptureViewer(self);
    resultobj = SWIG_NewPointerObj(SWIG_as_voidptr(result), SWIGTYPE_p_ICaptureViewer, SWIG_BUILTIN_INIT | 0);
    return Py_IsNone(resultobj) ? -1 : 0;
  }

  SWIGINTERN PyObject *capviewer_deinit(PyObject *self, PyObject *args)
  {
    PythonCaptureViewer *viewer = NULL;
    void *ptr = NULL;
    int res = SWIG_ConvertPtr(self, &ptr, SWIGTYPE_p_ICaptureViewer, SWIG_POINTER_DISOWN | 0);

    if(!SWIG_IsOK(res))
    {
      SWIG_exception_fail(SWIG_ArgError(res), "in method '" "delete_CaptureViewer" "', argument " "1"" of type '" "CaptureViewer *""'");
    }

    viewer = (PythonCaptureViewer *)ptr;
    delete viewer;

    return SWIG_Py_Void();
  fail:
    return NULL;
  }

SWIGPY_DESTRUCTOR_CLOSURE(capviewer_deinit) /* defines capviewer_deinit_destructor_closure */

%}

%feature("python:tp_init") ICaptureViewer "&capviewer_init";
%feature("python:tp_dealloc") ICaptureViewer "&capviewer_deinit_destructor_closure";


// need to ignore the original function and add a helper that releases the python GIL while calling
%ignore IReplayManager::BlockInvoke;

// ignore these functions as we don't map QVariantMap to/from python
%ignore EnvironmentModification::toJSON;
%ignore EnvironmentModification::fromJSON;

// rename the interfaces to remove the I prefix
%rename("%(regex:/^I([A-Z].*)/\\1/)s", %$isclass) "";

%{
  #ifndef slots
  #define slots
  #endif

  DECLARE_STRINGISE_TYPE(nbdstrpair);
%}

%include <stdint.i>

%include "Code/Interface/QRDInterface.h"
%include "Code/Interface/PersistantConfig.h"
%include "Code/Interface/RemoteHost.h"
%include "Code/Interface/Extensions.h"

DOCUMENT("");

TEMPLATE_ARRAY_INSTANTIATE(nbdarray, EventBookmark)
TEMPLATE_ARRAY_INSTANTIATE(nbdarray, ShaderProcessingTool)
TEMPLATE_ARRAY_INSTANTIATE(nbdarray, nbdstrpair)
TEMPLATE_ARRAY_INSTANTIATE(nbdarray, BugReport)
TEMPLATE_ARRAY_INSTANTIATE(nbdarray, ExtensionMetadata)
TEMPLATE_ARRAY_INSTANTIATE(nbdarray, DialogButton)
TEMPLATE_ARRAY_INSTANTIATE(nbdarray, RemoteHost)
TEMPLATE_ARRAY_INSTANTIATE_PTR(nbdarray, ICaptureViewer)

// unignore the function from above
%rename("%s") IReplayManager::BlockInvoke;

%extend IReplayManager {
  void BlockInvoke(InvokeCallback m) {
    PyObject *global_internal_handle = NULL;

    PyObject *globals = PyEval_GetGlobals();
    if(globals)
      global_internal_handle = PyDict_GetItemString(globals, "_noobdawn_internal");

    SetThreadBlocking(global_internal_handle, true);

    Py_BEGIN_ALLOW_THREADS
    $self->BlockInvoke(m);
    Py_END_ALLOW_THREADS

    SetThreadBlocking(global_internal_handle, false);
  }
};

%header %{
  #include <set>
  #include "Code/pynoobdawn/interface_check.h"

  // check interface, see interface_check.h for more information
  static swig_type_info **interfaceCheckTypes;
  static size_t interfaceCheckNumTypes = 0;

  bool CheckQtInterface(nbdstr &log)
  {
#if defined(RELEASE)
    return false;
#else
    if(interfaceCheckNumTypes == 0)
      return false;

    return check_interface(log, interfaceCheckTypes, interfaceCheckNumTypes);
#endif
  }
%}

%init %{
  interfaceCheckTypes = swig_type_initial;
  interfaceCheckNumTypes = sizeof(swig_type_initial)/sizeof(swig_type_initial[0]);
%}

// declare functions for using swig opaque wrap/unwrap of QWidget, for when pyside isn't available.
%wrapper %{

PyObject *WrapBareQWidget(QWidget *widget)
{
  return SWIG_InternalNewPointerObj(SWIG_as_voidptr(widget), SWIGTYPE_p_QWidget, 0);
}

QWidget *UnwrapBareQWidget(PyObject *obj)
{
  QWidget *ret = NULL;
  int res = 0;

  res = SWIG_ConvertPtr(obj, (void **)&ret,SWIGTYPE_p_QWidget, 0);
  if(!SWIG_IsOK(res))
  {
    return NULL;
  }

  return ret;
}

PythonCaptureViewer::PythonCaptureViewer(PyObject *s)
{
  self = s;
  Py_INCREF(self);
  
  StackExceptionHandler ex;

  PyObject *meth = NULL;

  {
    meth = PyObject_GetAttrString(self, "OnCaptureLoaded");
    if(PyCFunction_Check(meth) && PyCFunction_GetFunction(meth) == &_wrap_CaptureViewer_OnCaptureLoaded)
    {
      Py_XDECREF(meth);
      meth = NULL;
    }

    if(meth)
    {
      m_OnCaptureLoaded = ConvertFunc<std::function<void()>>("ICaptureViewer::OnCaptureLoaded", meth, ex);
      Py_XDECREF(meth);
    }
  }

  {
    meth = PyObject_GetAttrString(self, "OnCaptureClosed");
    if(PyCFunction_Check(meth) && PyCFunction_GetFunction(meth) == &_wrap_CaptureViewer_OnCaptureClosed)
    {
      Py_XDECREF(meth);
      meth = NULL;
    }

    if(meth)
    {
      m_OnCaptureClosed = ConvertFunc<std::function<void()>>("ICaptureViewer::OnCaptureClosed", meth, ex);
      Py_XDECREF(meth);
    }
  }

  {
    meth = PyObject_GetAttrString(self, "OnSelectedEventChanged");
    if(PyCFunction_Check(meth) && PyCFunction_GetFunction(meth) == &_wrap_CaptureViewer_OnSelectedEventChanged)
    {
      Py_XDECREF(meth);
      meth = NULL;
    }

    if(meth)
    {
      m_OnSelectedEventChanged = ConvertFunc<std::function<void(uint32_t)>>("ICaptureViewer::OnSelectedEventChanged", meth, ex);
      Py_XDECREF(meth);
    }
  }

  {
    meth = PyObject_GetAttrString(self, "OnEventChanged");
    if(PyCFunction_Check(meth) && PyCFunction_GetFunction(meth) == &_wrap_CaptureViewer_OnEventChanged)
    {
      Py_XDECREF(meth);
      meth = NULL;
    }

    if(meth)
    {
      m_OnEventChanged = ConvertFunc<std::function<void(uint32_t)>>("ICaptureViewer::OnEventChanged", meth, ex);
      Py_XDECREF(meth);
    }
  }
}

%}
