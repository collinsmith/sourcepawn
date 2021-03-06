// vim: set ts=2 sw=2 tw=99 et:
// 
// Copyright (C) 2012-2014 AlliedModders LLC and David Anderson
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
#include "compile-context.h"
#include "semantic-analysis.h"

namespace sp {

using namespace ke;
using namespace ast;

SemanticAnalysis::SemanticAnalysis(CompileContext &cc, TranslationUnit *tu)
 : cc_(cc),
   pool_(cc.pool()),
   types_(cc.types()),
   tu_(tu),
   funcstate_(nullptr)
{
}

sema::Program*
SemanticAnalysis::analyze()
{
  if (!walkAST())
    return nullptr;

  sema::Program* program = new (pool_) sema::Program;
  program->functions = ke::Move(global_functions_);
  return program;
}

bool
SemanticAnalysis::walkAST()
{
  ParseTree *tree = tu_->tree();
  StatementList *statements = tree->statements();
  for (size_t i = 0; i < statements->length(); i++) {
    Statement *stmt = statements->at(i);
    switch (stmt->kind()) {
      case AstKind::kFunctionStatement:
      {
        FunctionStatement* fun = stmt->toFunctionStatement();
        if (sema::FunctionDef* def = visitFunctionStatement(fun))
          global_functions_.append(def);
        break;
      }
      default:
        assert(false);
    }
    if (!cc_.canContinueProcessing())
      return false;
  }
  return cc_.phasePassed();
}

sema::FunctionDef*
SemanticAnalysis::visitFunctionStatement(FunctionStatement *node)
{
  FunctionSymbol *sym = node->sym();

  assert(!funcstate_);

  if (!funcstate_ && sym->shadows()) {
    // We are the root in a series of shadowed functions.
    analyzeShadowedFunctions(sym);
  }

  if (!node->body())
    return nullptr;

  FuncState state(&funcstate_, node);
  sema::Block* block = visitBlockStatement(node->body());

  return new (pool_) sema::FunctionDef(node, block);
}

// :TODO: write tests for this.
void
SemanticAnalysis::analyzeShadowedFunctions(FunctionSymbol *sym)
{
  // We do not yet support overloading, so two functions with the same name
  // and a body are illegal. We consider natives to be implemented.
  FunctionStatement *impl = nullptr;

  // We support non-native implementations of a forwarded function.
  FunctionStatement *forward = nullptr;

  for (size_t i = 0; i < sym->shadows()->length(); i++) {
    FunctionStatement *stmt = sym->shadows()->at(i);
    switch (stmt->token()) {
      case TOK_FORWARD:
        if (forward) {
          cc_.report(stmt->loc(), rmsg::function_redeclared)
            << stmt->name()
            << (cc_.note(forward->loc(), rmsg::previous_location));
          continue;
        }
        forward = stmt;
        break;
      case TOK_NATIVE:
      case TOK_FUNCTION:
        if (impl) {
          cc_.report(stmt->loc(), rmsg::function_redeclared)
            << stmt->name()
            << (cc_.note(impl->loc(), rmsg::previous_location));
          continue;
        }
        impl = stmt;
        break;
      default:
        assert(false);
        break;
    }
  }

  // If we have both an impl and a forward, make sure they match.
  if (impl && forward)
    checkForwardedFunction(forward, impl);
}

void
SemanticAnalysis::checkForwardedFunction(FunctionStatement *forward, FunctionStatement *impl)
{
  // SP1 didn't check these. We tighten up the semantics a bit for SP2.
  if (impl->token() == TOK_NATIVE) {
    cc_.report(impl->loc(), rmsg::illegal_forward_native)
      << impl->name()
      << cc_.note(forward->loc(), rmsg::previous_location);
    return;
  }
   
  if (!(impl->attrs() & DeclAttrs::Public)) {
    cc_.report(impl->loc(), rmsg::illegal_forward_func)
      << impl->name()
      << cc_.note(forward->loc(), rmsg::previous_location);
    return;
  }

  FunctionSignature *fwdSig = forward->signature();
  FunctionSignature *implSig = impl->signature();

  if (!matchForwardSignatures(fwdSig, implSig)) {
    cc_.report(impl->loc(), rmsg::forward_signature_mismatch)
      << impl->name()
      << cc_.note(forward->loc(), rmsg::previous_location);
    return;
  }
}

bool
SemanticAnalysis::matchForwardSignatures(FunctionSignature *fwdSig, FunctionSignature *implSig)
{
  // Due to SourceMod oddness, and the implementation detail that arguments are
  // pushed in reverse order, the impl function is allowed to leave off any
  // number of arguments. But, it cannot have more arguments.
  if (fwdSig->parameters()->length() < implSig->parameters()->length())
    return false;

  // We allow return types to differ iff the forward's type is void and the
  // impl function is implicit-int.
  Type *fwdRetType = fwdSig->returnType().resolved();
  Type *implRetType = implSig->returnType().resolved();
  if (!matchForwardReturnTypes(fwdRetType, implRetType))
    return false;

  return true;
}

bool
SemanticAnalysis::matchForwardReturnTypes(Type *fwdRetType, Type *implRetType)
{
  if (AreTypesEquivalent(fwdRetType, implRetType, Qualifiers::None))
    return true;
  if ((fwdRetType->isVoid() || fwdRetType->isImplicitInt()) && implRetType->isImplicitVoid())
    return true;
  return false;
}

sema::Block*
SemanticAnalysis::visitBlockStatement(BlockStatement* node)
{
  sema::Statements* stmts = new (pool_) sema::Statements();

  for (size_t i = 0; i < node->statements()->length(); i++) {
    Statement* ast_stmt = node->statements()->at(i);
    if (sema::Statement* stmt = visitStatement(ast_stmt))
      stmts->append(stmt);
  }

  return new (pool_) sema::Block(node, stmts);
}

sema::Statement*
SemanticAnalysis::visitStatement(Statement* node)
{
  switch (node->kind()) {
    case AstKind::kReturnStatement:
    {
      ReturnStatement* stmt = node->toReturnStatement();
      return visitReturnStatement(stmt);
    }
    default:
      assert(false);
  }
  return nullptr;
}

sema::Expr*
SemanticAnalysis::visitExpression(Expression* node)
{
  switch (node->kind()) {
    case AstKind::kIntegerLiteral:
    {
      IntegerLiteral* lit = node->toIntegerLiteral();
      return visitIntegerLiteral(lit);
    }
    default:
      assert(false);
  }
  return nullptr;
}

sema::Return*
SemanticAnalysis::visitReturnStatement(ReturnStatement* node)
{
  FunctionSignature* sig = funcstate_->sig;
  Type* returnType = sig->returnType().resolved();

  if (returnType->isVoid()) {
    if (node->expr())
      cc_.report(node->loc(), rmsg::returned_in_void_function);
    return new (pool_) sema::Return(node, nullptr);
  }

  if (!node->expr())
    cc_.report(node->loc(), rmsg::need_return_value);

  sema::Expr* expr = visitExpression(node->expr());

  if (!(expr = coerce(expr, returnType, Coercion::Return)))
    return nullptr;

  return new (pool_) sema::Return(node, expr);
}

sema::ConstValue*
SemanticAnalysis::visitIntegerLiteral(IntegerLiteral* node)
{
  // :TODO: test overflow
  int32_t value;
  if (!IntValue::SafeCast(node->value(), &value)) {
    cc_.report(node->loc(), rmsg::int_literal_out_of_range);
    return nullptr;
  }

  BoxedValue b(IntValue::FromValue(value));

  Type* i32type = types_->getPrimitive(PrimitiveType::Int32);
  return new (pool_) sema::ConstValue(node, i32type, b);
}

sema::Expr*
SemanticAnalysis::coerce(sema::Expr* expr, Type* to, Coercion context)
{
  Type* from = expr->type();

  if (from == to)
    return expr;

  if (from->isPrimitive() && to->isPrimitive()) {
    if (from->primitive() == to->primitive())
      return expr;
  }

  assert(false);
  return nullptr;
}

#if 0
void
SemanticAnalysis::visitBlockStatement(BlockStatement *node)
{
  StatementList *stmts = node->statements();
  for (size_t i = 0; i < stmts->length(); i++) {
    Statement *stmt = stmts->at(i);
    stmt->accept(this);
  }
}
#endif

#if 0
void
SemanticAnalysis::visitExpressionStatement(ExpressionStatement *node)
{
  Expression *expr = node->expr();

  expr->accept(this);

  // if (!expr->hasSideEffects())
  //   cc_.report(node->loc(), rmsg::expr_has_no_side_effects);
}
#endif

#if 0
void
SemanticAnalysis::visitCallExpr(CallExpr *node)
{
  // :TODO: we must verify that the callee is an implemented scripted func.
  Expression *callee = visitForRValue(node->callee());
  if (!callee)
    return;
  if (!callee->type()->isFunction()) {
    cc_.report(node->loc(), rmsg::callee_is_not_a_function)
      << callee->type();
    return;
  }
  node->setCallee(callee);

  FunctionSignature *sig = callee->type()->toFunction()->signature();
  checkCall(sig, node->arguments());

  Type *returnType = sig->returnType().resolved();
  node->setOutput(returnType, VK::rvalue);

  // We mark calls as always having side effects.
  node->setHasSideEffects();
}
#endif

void
SemanticAnalysis::checkCall(FunctionSignature *sig, ExpressionList *args)
{
  VarDecl *vararg = nullptr;
  for (size_t i = 0; i < args->length(); i++) {
    Expression *expr = args->at(i);

    VarDecl *arg = nullptr;
    if (i >= sig->parameters()->length()) {
      if (!vararg) {
        cc_.report(expr->loc(), rmsg::wrong_argcount)
          << args->length(), sig->parameters()->length();
        return;
      }
      arg = vararg;
    } else {
      arg = sig->parameters()->at(i);
    }
    (void)arg;

#if 0
    visitForValue(expr);

    Coercion cr(cc_,
                Coercion::Reason::arg,
                expr,
                arg->te().resolved());
    if (cr.coerce() != Coercion::Result::ok) {
      auto builder = cc_.report(expr->loc(), rmsg::cannot_coerce_for_arg)
        << expr->type()
        << arg->te().resolved();

      if (i < args->length() && arg->name())
        builder << arg->name();
      else
        builder << i;

      builder << cr.diag(expr->loc());
      break;
    }

    // Rewrite the tree for the coerced result.
    args->at(i) = cr.output();
#endif
  }
}

#if 0
void
SemanticAnalysis::visitNameProxy(NameProxy *proxy)
{
  Symbol *binding = proxy->sym();
  switch (binding->kind()) {
    case Symbol::kType:
      cc_.report(proxy->loc(), rmsg::cannot_use_type_as_value)
        << binding->asType()->type();
      break;

    case Symbol::kConstant:
    {
      //ConstantSymbol *sym = binding->toConstant();
      //proxy->setOutput(sym->type(), VK::rvalue);
      break;
    }

    case Symbol::kFunction:
    {
      FunctionSymbol *sym = binding->toFunction();
      FunctionStatement *decl = sym->impl();
      if (!decl) {
        cc_.report(proxy->loc(), rmsg::function_has_no_impl)
          << sym->name();
        break;
      }

      if (!decl->type())
        decl->setType(FunctionType::New(decl->signature()));

      // Function symbols are clvalues, since they are named.
      // :TODO:
      proxy->setOutput(decl->type(), VK::lvalue);
      break;
    }

    default:
      assert(false);
  }
}
#endif

#if 0
void
SemanticAnalysis::visitStringLiteral(StringLiteral *node)
{
  // Build a constant array for the character string.
  Type *charType = types_->getPrimitive(PrimitiveType::Char);
  ArrayType *arrayType = types_->newArray(charType, node->arrayLength());
  Type *litType = types_->newQualified(arrayType, Qualifiers::Const);

  // Returned value is always an rvalue.
  node->setOutput(litType, VK::rvalue);
}
#endif

#if 0
void
SemanticAnalysis::visitReturnStatement(ReturnStatement *node)
{
  assert(funcstate_ && funcstate_->sig);

  //Type *retType = funcstate_->sig->returnType().resolved();
  //if (retType->isVoid() || retType->isImplicitVoid())
}
#endif

} // namespace sp
