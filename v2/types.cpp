// vim: set sts=2 ts=8 sw=2 tw=99 et:
//
// Copyright (C) 2012-2014 David Anderson
//
// This file is part of SourcePawn.
//
// SourcePawn is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
// 
// SourcePawn is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// SourcePawn. If not, see http://www.gnu.org/licenses/.
#include "types.h"
#include "compile-context.h"

using namespace ke;

Type *
Type::NewVoid()
{
  Type *type = new (POOL()) Type(VOID);
  return type;
}

Type *
Type::NewUnchecked()
{
  Type *type = new (POOL()) Type(UNCHECKED);
  return type;
}

Type *
Type::NewPrimitive(PrimitiveType prim)
{
  Type *type = new (POOL()) Type(PRIMITIVE);
  type->primitive_ = prim;
  return type;
}

ArrayType *
ArrayType::New(Type *contained, int elements)
{
  ArrayType *type = new (POOL()) ArrayType(ARRAY);
  type->contained_ = contained;
  type->elements_ = elements;
  return type;
}

ReferenceType *
ReferenceType::New(Type *contained)
{
  ReferenceType *type = new (POOL()) ReferenceType();

  assert(!contained->isReference());

  type->contained_ = contained;
  return type;
}

EnumType *
EnumType::New(Atom *name)
{
  EnumType *type = new (POOL()) EnumType();
  type->kind_ = ENUM;
  type->name_ = name;
  return type;
}

#if 0
Type *
Type::NewNative(Type *returnType, Handle<FixedArray> parameters,
        Handle<FixedArray> defaults, bool variadic)
{
  Local<Type> type(zone, Type::cast(zone->allocate(MapKind_Type, sizeof(Type), Heap::Tenure_Old)));
  if (!type)
    return nullptr;

  type->kind_ = NATIVE;
  type->name_ = nullptr;
  type->contained_ = nullptr;
  type->fields_ = nullptr;
  type->newMap_ = nullptr;
  type->returnType_ = returnType;
  type->parameters_ = parameters;
  type->defaults_ = defaults;
  type->variadicNative_ = variadic;
  return type;
}
#endif

bool
Type::Compare(Type *left, Type *right)
{
  if (left == right)
    return true;

  if (left->kind_ != right->kind_)
    return false;

  switch (left->kind_) {
    case Type::PRIMITIVE:
      return left->primitive() == right->primitive();

    case Type::ARRAY:
    {
      ArrayType *aleft = left->toArray();
      ArrayType *aright = right->toArray();
      if (aleft->size() != aright->size())
        return false;
      return Compare(aleft->contained(), aright->contained());
    }

    case Type::FUNCTION:
      // :TODO:
      return false;

    case Type::ENUM:
    case Type::TYPEDEF:
      return false;

    case Type::VOID:
      return true;

    default:
      assert(left->kind_ == Type::REFERENCE);
      return Compare(left->toReference()->contained(), right->toReference()->contained());
  }
}

FunctionType *
FunctionType::New(FunctionSignature *sig)
{
  return new (POOL()) FunctionType(sig);
}

const char *
ke::GetPrimitiveName(PrimitiveType type)
{
  switch (type) {
    case PrimitiveType::Bool:
      return "bool";
    case PrimitiveType::Char:
      return "char";
    case PrimitiveType::Int32:
      return "int";
    case PrimitiveType::Float:
      return "float";
    default:
      assert(false);
      return "unknown";
  }
}

const char *
ke::GetTypeName(Type *type)
{
  if (type->isArray())
    return "array";
  if (type->isFunction())
    return "function";
  if (type->isVoid())
    return "void";
  if (type->isEnum())
    return type->toEnum()->name()->chars();
  if (type->isReference())
    type = type->toReference()->contained();
  return GetPrimitiveName(type->primitive());
}

