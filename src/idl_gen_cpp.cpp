/*
 * Copyright 2014 Google Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// independent from idl_parser, since this code is not needed for most clients

#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/idl.h"
#include "flatbuffers/util.h"

#include <string>
using namespace std;

namespace flatbuffers {
namespace cpp {

// Return a C++ type from the table in idl.h
static std::string GenTypeBasic(const Type &type) {
  static const char *ctypename[] = {
    #define FLATBUFFERS_TD(ENUM, IDLTYPE, CTYPE, JTYPE) #CTYPE,
      FLATBUFFERS_GEN_TYPES(FLATBUFFERS_TD)
    #undef FLATBUFFERS_TD
  };
  return ctypename[type.base_type];
}

static std::string GenTypeWire(const Type &type, const char *postfix);

// Return a C++ pointer type, specialized to the actual struct/table types,
// and vector element types.
static std::string GenTypePointer(const Type &type) {
  switch (type.base_type) {
    case BASE_TYPE_STRING:
      return "fb_string";
    case BASE_TYPE_VECTOR:
      return "fb_vector<" + GenTypeWire(type.VectorType(), "") + ">";
    case BASE_TYPE_STRUCT:
      return type.struct_def->name;
    case BASE_TYPE_UNION:
      // fall through
    default:
      return "void";
  }
}

// Return a C++ type for any type (scalar/pointer) specifically for
// building a flatbuffer.
static std::string GenTypeWire(const Type &type, const char *postfix) {
  return IsScalar(type.base_type)
    ? GenTypeBasic(type) + postfix
    : IsStruct(type)
      ? "const " + GenTypePointer(type) + " *"
      : "fb_offset<" + GenTypePointer(type) + ">" + postfix;
}

// Return a C++ type for any type (scalar/pointer) specifically for
// using a flatbuffer.
static std::string GenTypeGet(const Type &type, const char *afterbasic,
                              const char *beforeptr, const char *afterptr) {
  return IsScalar(type.base_type)
    ? GenTypeBasic(type) + afterbasic
    : beforeptr + GenTypePointer(type) + afterptr;
}

// Generate a documentation comment, if available.
static void GenComment(const std::string &dc,
                       std::string *code_ptr,
                       const char *prefix = "") {
  std::string &code = *code_ptr;
  if (dc.length()) {
    code += std::string(prefix) + "///" + dc + "\n";
  }
}

// Generate an enum declaration and an enum string lookup table.
static void GenEnum(EnumDef &enum_def, std::string *code_ptr) {
  if (enum_def.generated) return;
  std::string &code = *code_ptr;
  GenComment(enum_def.doc_comment, code_ptr);
  code += "enum\n{\n";
  for (auto it = enum_def.vals.vec.begin();
       it != enum_def.vals.vec.end();
       ++it) {
    auto &ev = **it;
    GenComment(ev.doc_comment, code_ptr, "  ");
    code += "\t" + enum_def.name + "_" + ev.name + " = ";
    code += NumToString(ev.value) + ",\n";
  }
  code += "};\n\n";

  // Generate a generate string table for enum values.
  // Problem is, if values are very sparse that could generate really big
  // tables. Ideally in that case we generate a map lookup instead, but for
  // the moment we simply don't output a table at all.
  int range = enum_def.vals.vec.back()->value -
              enum_def.vals.vec.front()->value + 1;
  // Average distance between values above which we consider a table
  // "too sparse". Change at will.
  static const int kMaxSparseness = 5;
  if (range / static_cast<int>(enum_def.vals.vec.size()) < kMaxSparseness) {
    code += "inline const char **EnumNames" + enum_def.name + "()\n{\n";
    code += "\tstatic const char *names[] = { ";
    int val = enum_def.vals.vec.front()->value;
    for (auto it = enum_def.vals.vec.begin();
         it != enum_def.vals.vec.end();
         ++it) {
      while (val++ != (*it)->value) code += "\"\", ";
      code += "\"" + (*it)->name + "\", ";
    }
    code += "nullptr};\n\treturn names;\n}\n\n";
    code += "inline const char *EnumName" + enum_def.name;
    code += "(int e)\n{\n\treturn EnumNames" + enum_def.name + "()[e";
    if (enum_def.vals.vec.front()->value)
      code += " - " + enum_def.name + "_" + enum_def.vals.vec.front()->name;
    code += "];\n}\n\n";
  }
}

// Generate an accessor struct, builder structs & function for a table.
static void GenTable(StructDef &struct_def, std::string *code_ptr) {
  if (struct_def.generated) return;
  std::string &code = *code_ptr;

  // Generate an accessor struct, with methods of the form:
  // type name() const { return GetField<type>(offset, defaultval); }
  GenComment(struct_def.doc_comment, code_ptr);
  code += "struct " + struct_def.name + " : private fb_table";
  code += "\n{";
  for (auto it = struct_def.fields.vec.begin();
       it != struct_def.fields.vec.end();
       ++it) {
    auto &field = **it;
    if (!field.deprecated) {  // Deprecated fields won't be accessible.
      GenComment(field.doc_comment, code_ptr, "  ");
      code += "\n\t" + GenTypeGet(field.value.type, " ", "const ", " *");
      code += field.name + "() const\n\t{\n\t\treturn ";
      // Call a different accessor for pointers, that indirects.
      code += IsScalar(field.value.type.base_type)
        ? "GetField<"
        : (IsStruct(field.value.type) ? "GetStruct<" : "GetPointer<");
      code += GenTypeGet(field.value.type, "", "const ", " *") + ">(";
      code += NumToString(field.value.offset);
      // Default value as second arg for non-pointer types.
      if (IsScalar(field.value.type.base_type))
        code += ", " + field.value.constant;
      code += ");\n\t}\n";
    }
  }
  code += "};\n\n";

  // Generate a builder struct, with methods of the form:
  // void add_name(type name) { fbb_.AddElement<type>(offset, name, default); }
  code += "struct " + struct_def.name;
  code += "_builder\n{\n\tfb_builder &fbb_;\n";
  code += "\tfb::uoffset_t start_;\n";
  for (auto it = struct_def.fields.vec.begin();
       it != struct_def.fields.vec.end();
       ++it) {
    auto &field = **it;
    if (!field.deprecated) {
      code += "\n\tvoid add_" + field.name + "(";
      code += GenTypeWire(field.value.type, " ") + field.name + ")\n\t{\n\t\tfbb_.Add";
      if (IsScalar(field.value.type.base_type))
        code += "Element<" + GenTypeWire(field.value.type, "") + ">";
      else if (IsStruct(field.value.type))
        code += "Struct";
      else
        code += "Offset";
      code += "(" + NumToString(field.value.offset) + ", " + field.name;
      if (IsScalar(field.value.type.base_type))
        code += ", " + field.value.constant;
      code += ");\n\t}\n";
    }
  }
  code += "\n\t" + struct_def.name;
  code += "_builder(fb_builder &_fbb) : fbb_(_fbb)";
  code += "\n\t{\n\t\tstart_ = fbb_.StartTable();\n\t}\n";
  code += "\n\tfb_offset<" + struct_def.name;
  code += "> Finish()\n\t{\n\t\treturn fb_offset<" + struct_def.name;
  code += ">(fbb_.EndTable(start_, ";
  code += NumToString(struct_def.fields.vec.size()) + "));\n\t}\n};\n\n";

  // Generate a convenient CreateX function that uses the above builder
  // to create a table in one go.
  code += "inline fb_offset<" + struct_def.name + "> create_";
  code += struct_def.name;
  code += "(\n\tfb_builder &_fbb";
  for (auto it = struct_def.fields.vec.begin();
       it != struct_def.fields.vec.end();
       ++it) {
    auto &field = **it;
    if (!field.deprecated) {
      code += ",\n\t" + GenTypeWire(field.value.type, " ") + field.name;
    }
  }
  code += ")\n{\n\t" + struct_def.name + "_builder builder_(_fbb);\n";
  for (size_t size = struct_def.sortbysize ? sizeof(largest_scalar_t) : 1;
       size;
       size /= 2) {
    for (auto it = struct_def.fields.vec.rbegin();
         it != struct_def.fields.vec.rend();
         ++it) {
      auto &field = **it;
      if (!field.deprecated &&
          (!struct_def.sortbysize ||
           size == SizeOf(field.value.type.base_type))) {
        code += "\tbuilder_.add_" + field.name + "(" + field.name + ");\n";
      }
    }
  }
  code += "\treturn builder_.Finish();\n}\n\n";
}

// Generate an accessor struct with constructor for a flatbuffers struct.
static void GenStruct(StructDef &struct_def, std::string *code_ptr) {
  if (struct_def.generated) return;
  std::string &code = *code_ptr;

  // Generate an accessor struct, with private variables of the form:
  // type name_;
  // Generates manual padding and alignment.
  // Variables are private because they contain little endian data on all
  // platforms.
  GenComment(struct_def.doc_comment, code_ptr);
  code += "MANUALLY_ALIGNED_STRUCT(" + NumToString(struct_def.minalign) + ") ";
  code += struct_def.name + "\n{\n private:\n";
  int padding_id = 0;
  for (auto it = struct_def.fields.vec.begin();
       it != struct_def.fields.vec.end();
       ++it) {
    auto &field = **it;
    code += "  " + GenTypeGet(field.value.type, " ", "", " ");
    code += field.name + "_;\n";
    if (field.padding) {
      for (int i = 0; i < 4; i++)
        if (static_cast<int>(field.padding) & (1 << i))
          code += "  int" + NumToString((1 << i) * 8) +
                  "_t __padding" + NumToString(padding_id++) + ";\n";
      assert(!(field.padding & ~0xF));
    }
  }

  // Generate a constructor that takes all fields as arguments.
  code += "\n public:\n  " + struct_def.name + "(";
  for (auto it = struct_def.fields.vec.begin();
       it != struct_def.fields.vec.end();
       ++it) {
    auto &field = **it;
    if (it != struct_def.fields.vec.begin()) code += ", ";
    code += GenTypeGet(field.value.type, " ", "const ", " &") + field.name;
  }
  code += ")\n    : ";
  padding_id = 0;
  for (auto it = struct_def.fields.vec.begin();
       it != struct_def.fields.vec.end();
       ++it) {
    auto &field = **it;
    if (it != struct_def.fields.vec.begin()) code += ", ";
    code += field.name + "_(";
    if (IsScalar(field.value.type.base_type))
      code += "fb::EndianScalar(" + field.name + "))";
    else
      code += field.name + ")";
    if (field.padding)
      code += ", __padding" + NumToString(padding_id++) + "(0)";
  }
  code += "\n{\n}\n\n";

  // Generate accessor methods of the form:
  // type name() const { return fb::EndianScalar(name_); }
  for (auto it = struct_def.fields.vec.begin();
       it != struct_def.fields.vec.end();
       ++it) {
    auto &field = **it;
    GenComment(field.doc_comment, code_ptr, "  ");
    code += "  " + GenTypeGet(field.value.type, " ", "const ", " &");
    code += field.name + "() const\n{\nreturn ";
    if (IsScalar(field.value.type.base_type))
      code += "fb::EndianScalar(" + field.name + "_)";
    else
      code += field.name + "_";
    code += ";\n\t}\n";
  }
  code += "\n};\nSTRUCT_END(" + struct_def.name + ", ";
  code += NumToString(struct_def.bytesize) + ");\n\n";
}

}  // namespace cpp

// Iterate through all definitions we haven't generate code for (enums, structs,
// and tables) and output them to a single file.
std::string GenerateCPP(const Parser &parser) {
  using namespace cpp;

  // Generate code for all the enum declarations.
  std::string enum_code;
  for (auto it = parser.enums_.vec.begin();
       it != parser.enums_.vec.end(); ++it) {
    GenEnum(**it, &enum_code);
  }

  // Generate forward declarations for all structs/tables, since they may
  // have circular references.
  std::string forward_decl_code;
  for (auto it = parser.structs_.vec.begin();
       it != parser.structs_.vec.end(); ++it) {
    if (!(*it)->generated)
      forward_decl_code += "struct " + (*it)->name + ";\n";
  }

  // Generate code for all structs, then all tables.
  std::string decl_code;
  for (auto it = parser.structs_.vec.begin();
       it != parser.structs_.vec.end(); ++it) {
    if ((**it).fixed) GenStruct(**it, &decl_code);
  }
  for (auto it = parser.structs_.vec.begin();
       it != parser.structs_.vec.end(); ++it) {
    if (!(**it).fixed) GenTable(**it, &decl_code);
  }

  // Only output file-level code if there were any declarations.
  if (enum_code.length() || forward_decl_code.length() || decl_code.length()) {
    std::string code;
    code = "\n";
    code += "#include \"flatbuffers/flatbuffers.h\"\n";
    code += "\nnamespace fb = flatbuffers;\n";
    code += "\n#define fb_offset                 fb::Offset";
    code += "\n#define fb_string                 fb::String";
    code += "\n#define fb_vector                 fb::Vector";
    code += "\n#define fb_table                  fb::Table";
    code += "\n#define fb_builder                fb::FlatBufferBuilder";
    code += "\n#define fb_create_string(b, ...)  (b).CreateString(__VA_ARGS__)";
    code += "\n#define fb_create_vector(b, ...)  (b).CreateVector(__VA_ARGS__)";
    code += "\n#define fb_vector_size(v)         (unsigned)(*(v)).Length()";
    code += "\n#define fb_vector_length(v)       (unsigned)(*(v)).Length()";
    code += "\n#define fb_vector_at(v, i)        (*(v)).Get(i)";
    code += "\n#define fb_get_buf(b)             (b).GetBufferPointer()";
    code += "\n#define fb_get_size(b)            (unsigned)(b).GetSize()";
    code += "\n#define fb_clear(b)               (b).Clear()";
    code += "\n#define fb_finish(b, buf)         (b).Finish(buf)\n";

    for (auto it = parser.name_space_.begin();
         it != parser.name_space_.end(); ++it) {
      code += "\nnamespace " + *it + "\n{\n";
    }
    code += "\n";
    code += enum_code;
    code += forward_decl_code;
    code += "\n";
    code += decl_code;
    if (parser.root_struct_def) {
      code += "inline const " + parser.root_struct_def->name + " *get_";
      code += parser.root_struct_def->name;
      code += "(const void *buf)\n{\n\treturn fb::GetRoot<";
      code += parser.root_struct_def->name + ">(buf);\n}\n";
    }
    for (auto it = parser.name_space_.begin();
         it != parser.name_space_.end(); ++it) {
      code += "\n}; // namespace " + *it + "\n";
    }

    return code;
  }

  return std::string();
}

bool GenerateCPP(const Parser &parser,
                 const std::string &path,
                 const std::string &file_name) {
    auto file_name_macro = "__" + file_name + "_FLATBUFFERS_H__";
    transform(file_name_macro.begin(), file_name_macro.end(), file_name_macro.begin(), (int(*)(int))toupper);
    auto code = string("// automatically generated, do not modify\n\n")
               + "#ifndef " + file_name_macro + "\n"
               + "#define " + file_name_macro + "\n\n"
               + GenerateCPP(parser)
               + "\n#endif\n"
               + "\n// the end of the header file "
               + file_name
               + ".fb.h\n\n";
    return !code.length() ||
           SaveFile((path + file_name + ".fb.h").c_str(), code, false);
}

}  // namespace flatbuffers

