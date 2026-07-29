/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2017-2026 Baldur Karlsson
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

// verify the interface.
// We check that docstrings aren't duplicated, which is a symptom of missing DOCUMENT()
// macros around newly added classes/members.
// For enums, verify that all constants are documented in the parent docstring
// Generally we ensure naming is roughly OK:
// * types, member functions must match the regexp /[A-Z][a-zA-Z0-9]+/
//   ie. we don't use underscore_seperated_words or mixedCase / camelCase.
// * Enum values are similar that they must start with a capital, but we allow underscores.
// * data members should be mixedCase / camelCase. So matching /[a-z][a-zA-Z0-9]+/
// This isn't quite python standards but it fits best with the C++ code and the important
// thing is that it's self-consistent.

enum class NameType
{
  Type,
  EnumValue,
  Method,
  Member,
};

inline bool checkname(nbdstr &log, const char *baseType, nbdstr name, NameType nameType,
                      const nbdstr &mem_doc)
{
  // skip __ prefixed names
  if(name.beginsWith("__"))
    return false;

  // skip any nbdtype based types that are converted into equivalent python types
  if((baseType && strstr(baseType, "nbdarray")) || name.contains("nbdarray"))
    return false;
  if((baseType && strstr(baseType, "bytebuf")) || name.contains("bytebuf"))
    return false;
  if((baseType && strstr(baseType, "nbdstr")) || name.contains("nbdstr"))
    return false;
  if((baseType && strstr(baseType, "StructuredBufferList")) ||
     name.contains("StructuredBufferList"))
    return false;
  if((baseType && strstr(baseType, "StructuredChunkList")) || name.contains("StructuredChunkList"))
    return false;
  if((baseType && strstr(baseType, "StructuredObjectList")) ||
     name.contains("StructuredObjectList"))
    return false;

  // allow the config to have different names
  if((baseType && strstr(baseType, "PersistantConfig")) || name.contains("PersistantConfig"))
    return false;

  // skip swig internal type
  if((baseType && strstr(baseType, "SwigPyObject")) || name.contains("SwigPyObject"))
    return false;

  // remove the module prefix, if this is a type name we're checking
  if(name.beginsWith("noobdawn."))
    name.erase(0, 10);
  if(name.beginsWith("qnoobdawn."))
    name.erase(0, 11);

  // skip a few well-known members
  if(name == "this" || name == "thisown")
    return false;

  bool member = (nameType == NameType::Member);

  // look for invalid name
  bool badfirstChar = false;
  if(member)
    badfirstChar = name[0] < 'a' || name[0] > 'z';
  else
    badfirstChar = name[0] < 'A' || name[0] > 'Z';

  if(badfirstChar || (nameType != NameType::EnumValue && name.contains('_')))
  {
    const char *nameTypeStr = "";

    switch(nameType)
    {
      case NameType::EnumValue: nameTypeStr = "enum value"; break;
      case NameType::Member: nameTypeStr = "member variable"; break;
      case NameType::Method: nameTypeStr = "method"; break;
      case NameType::Type: nameTypeStr = "type"; break;
    }

    snprintf(convert_error, sizeof(convert_error) - 1,
             "Name of %s '%s.%s' does not match naming scheme.\n"
             "Should start with %s letter and not contain underscores\n",
             nameTypeStr, baseType, name.c_str(), member ? "lowercase" : "uppercase");
    log += convert_error;

    return true;
  }

  if(member)
  {
    if(!mem_doc.contains(":type:"))
    {
      snprintf(convert_error, sizeof(convert_error) - 1,
               "%s.%s is missing a :type: in its docstring\n", baseType, name.c_str());
      log += convert_error;

      return true;
    }
  }

  return false;
}

inline bool check_interface(nbdstr &log, swig_type_info **swig_types, size_t numTypes)
{
  // track all errors and fatal error at the end, so we see all of the problems at once instead of
  // requiring rebuilds over and over.
  // This does mean that e.g. a duplicated docstring could be reported multiple times but that's not
  // the end of the world.
  bool errors_found = false;

  std::set<nbdstr> struct_docstrings;
  for(size_t i = 0; i < numTypes; i++)
  {
    SwigPyClientData *typeinfo = (SwigPyClientData *)swig_types[i]->clientdata;

    // opaque types have no typeinfo, skip these
    if(!typeinfo)
      continue;

    PyTypeObject *typeobj = typeinfo->pytype;

    nbdstr typedoc = typeobj->tp_doc;

    auto result = struct_docstrings.insert(typedoc);

    if(!result.second)
    {
      snprintf(convert_error, sizeof(convert_error) - 1,
               "Duplicate docstring '%s' found on struct '%s' - are you missing a DOCUMENT()?\n",
               typedoc.c_str(), typeobj->tp_name);
      log += convert_error;
      errors_found = true;
    }

    nbdstr typeName = typeobj->tp_name;
    errors_found |= checkname(log, "noobdawn", typeName, NameType::Type, "");

    PyObject *dict = typeobj->tp_dict;

    if(!dict && typeobj->tp_base)
      dict = typeobj->tp_base->tp_dict;

    // check the object's dict to see if this is an enum (or struct with constants).
    // We require ALL constants be documented in a docstring with data:: directives
    if(dict && PyDict_Check(dict))
    {
      PyObject *keys = PyDict_Keys(dict);

      if(keys)
      {
        std::set<nbdstr> constants;

        Py_ssize_t len = PyList_Size(keys);
        for(Py_ssize_t i = 0; i < len; i++)
        {
          PyObject *key = PyList_GetItem(keys, i);
          PyObject *value = PyDict_GetItem(dict, key);

          // if this key is a string (it should be)
          if(PyUnicode_Check(key))
          {
            char *str = NULL;
            Py_ssize_t len = 0;
            PyObject *bytes = PyUnicode_AsUTF8String(key);
            PyBytes_AsStringAndSize(bytes, &str, &len);

            if(str == NULL || len == 0)
            {
              snprintf(convert_error, sizeof(convert_error) - 1,
                       "Couldn't get member name for %i'th member of '%s'\n", (int)i,
                       typeobj->tp_name);
              log += convert_error;
              errors_found = true;
            }
            else if(str[0] == '_')
            {
              // ignore leading underscores. We assume that we don't do this ourselves so this lets
              // us skip any internals we might get from looking at an Enum or IntFlag dict
            }
            else
            {
              nbdstr name(str, len);

              NameType nameType = NameType::Member;

              nbdstr mem_doc;

              // if the value is an integer, it's a constant
              if(PyLong_Check(value))
              {
                constants.insert(name);
                nameType = NameType::EnumValue;
              }
              else if(PyObject_HasAttrString(value, "__doc__"))
              {
                PyObject *doc = PyObject_GetAttrString(value, "__doc__");

                if(doc)
                {
                  if(!Py_IsNone(doc))
                  {
                    char *docstr = NULL;
                    Py_ssize_t doclen = 0;
                    PyObject *docbytes = PyUnicode_AsUTF8String(doc);
                    PyBytes_AsStringAndSize(docbytes, &docstr, &doclen);

                    mem_doc = nbdstr(docstr, doclen);

                    Py_DecRef(docbytes);
                  }
                  Py_DecRef(doc);
                }
              }

              // if it's a callable it's a method, ignore it
              if(!PyCallable_Check(value) && !PyType_IsSubtype(Py_TYPE(value), &PyStaticMethod_Type))
              {
                // some hardcoded exclusions that we allow to break the naming scheme
                if(typeName == "KnownShaderTool")
                {
                  // these tools refer to executables so it would be strange to capitalise them.
                }
                else if(typeName == "GPUVendor" && name == "nVidia")
                {
                  // nVidia's capitalisation is more consistent this way than Nvidia
                }
                else
                {
                  errors_found |= checkname(log, typeobj->tp_name, name, nameType, mem_doc);
                }
              }
            }

            Py_DecRef(bytes);
          }
        }

        Py_DecRef(keys);

        if(!constants.empty())
        {
          std::set<nbdstr> documented;

          const char identifier[] = ".. data::";

          int32_t offs = typedoc.find(identifier);

          while(offs >= 0)
          {
            offs += sizeof(identifier) - 1;

            while(isspace(typedoc[offs]))
              offs++;

            int32_t eol = typedoc.indexOf('\n', offs);

            if(eol < 0)
              break;

            documented.insert(typedoc.substr(offs, eol - offs));

            offs = typedoc.find(identifier, offs);
          }

          for(auto it = constants.begin(); it != constants.end(); ++it)
          {
            // allow enums with First or Count members to be undocumented
            if(*it == "First" || *it == "Count")
              continue;

            if(documented.find(*it) == documented.end())
            {
              snprintf(convert_error, sizeof(convert_error) - 1,
                       "'%s::%s' is not documented in class docstring\n", typeobj->tp_name,
                       it->c_str());
              log += convert_error;
              errors_found = true;
            }
          }
        }
      }
    }

    PyMethodDef *method = typeobj->tp_methods;

    std::set<nbdstr> method_docstrings;
    while(method->ml_doc)
    {
      nbdstr method_doc = method->ml_doc;

      errors_found |= checkname(log, typeobj->tp_name, method->ml_name, NameType::Method, "");

      int32_t i = 0;
      while(method_doc[i] == '\n')
        i++;

      // skip the first line as it's autodoc generated
      i = method_doc.find('\n', i);
      if(i >= 0)
      {
        while(method_doc[i] == '\n')
          i++;

        method_doc.erase(0, i);

        result = method_docstrings.insert(method_doc);

        if(!result.second)
        {
          snprintf(
              convert_error, sizeof(convert_error) - 1,
              "Duplicate docstring '%s' found on method '%s.%s' - are you missing a DOCUMENT()?\n",
              method_doc.c_str(), typeobj->tp_name, method->ml_name);
          log += convert_error;
          errors_found = true;
        }
      }

      method++;
    }

    PyGetSetDef *getset = typeobj->tp_getset;
    while(getset && getset->doc)
    {
      nbdstr getset_doc = getset->doc;

      result = method_docstrings.insert(getset_doc);

      if(!result.second)
      {
        snprintf(
            convert_error, sizeof(convert_error) - 1,
            "Duplicate docstring '%s' found on getset '%s.%s' - are you missing a DOCUMENT()?\n",
            getset_doc.c_str(), typeobj->tp_name, getset->name);
        log += convert_error;
        errors_found = true;
      }

      getset++;
    }
  }

  return errors_found;
}
