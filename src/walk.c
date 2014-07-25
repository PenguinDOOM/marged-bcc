#include "task.h"

#include <stdio.h>

struct operand {
   struct type* type;
   struct dim* dim;
   enum {
      ACTION_PUSH_VALUE,
      ACTION_PUSH_VAR
   } action;
   enum {
      METHOD_NONE,
      METHOD_INDEXED,
      METHOD_ELEMENT
   } method;
   enum {
      TRIGGER_INDEX
   } trigger;
   int storage;
   int index;
   int base;
   bool push;
   bool push_temp;
   bool pushed;
   bool pushed_element;
};

struct block_walk {
   struct block_walk* prev;
   struct format_block_usage* format_block_usage;
   int size;
   int size_high;
};

static void do_block( struct task*, struct block* );
static void add_block_walk( struct task* );
static void pop_block_walk( struct task* );
static int alloc_scalar( struct task* );
static void dealloc_last_scalar( struct task* );
static void do_var( struct task*, struct var* );
static void do_world_global_init( struct task*, struct var* );
static void do_node( struct task*, struct node* );
static void do_script_jump( struct task*, struct script_jump* );
static void do_label( struct task*, struct label* );
static void do_goto( struct task*, struct goto_stmt* );
static void init_operand( struct operand* );
static void push_expr( struct task*, struct expr*, bool );
static void do_expr( struct task*, struct expr* );
static void do_expr_stmt( struct task*, struct packed_expr* );
static void do_operand( struct task*, struct operand*, struct node* );
static void do_constant( struct task*, struct operand*, struct constant* );
static void do_unary( struct task*, struct operand*, struct unary* );
static void do_pre_inc( struct task*, struct operand*, struct unary* );
static void do_post_inc( struct task*, struct operand*, struct unary* );
static void do_call( struct task*, struct operand*, struct call* );
static void do_format_call( struct task*, struct call*, struct func* );
static void do_format_item( struct task*, struct format_item* );
static void do_binary( struct task*, struct operand*, struct binary* );
static void do_assign( struct task*, struct operand*, struct assign* );
static void do_var_name( struct task*, struct operand*, struct var* );
static void set_var( struct task*, struct operand*, struct var* );
static void do_object( struct task*, struct operand*, struct node* );
static void do_subscript( struct task*, struct operand*, struct subscript* );
static void do_access( struct task*, struct operand*, struct access* );
static void push_indexed( struct task*, int, int );
static void push_element( struct task*, int, int );
static void inc_indexed( struct task*, int, int );
static void dec_indexed( struct task*, int, int );
static void inc_element( struct task*, int, int );
static void dec_element( struct task*, int, int );
static void update_indexed( struct task*, int, int, int );
static void update_element( struct task*, int, int, int );
static void do_if( struct task*, struct if_stmt* );
static void do_switch( struct task*, struct switch_stmt* );
static void do_case( struct task*, struct case_label* );
static void do_while( struct task*, struct while_stmt* );
static void do_for( struct task*, struct for_stmt* );
static void do_jump( struct task*, struct jump* );
static void do_jump_target( struct task*, struct jump*, int );
static void do_return( struct task*, struct return_stmt* );
static void do_paltrans( struct task*, struct paltrans* );
static void do_default_params( struct task*, struct func* );

static const int g_aspec_code[] = {
   PCD_LSPEC1,
   PCD_LSPEC2,
   PCD_LSPEC3,
   PCD_LSPEC4,
   PCD_LSPEC5
};

void t_publish_usercode( struct task* task ) {
   // Scripts.
   list_iter_t i;
   list_iter_init( &i, &task->library_main->scripts );
   while ( ! list_end( &i ) ) {
      struct script* script = list_data( &i );
      script->offset = t_tell( task );
      add_block_walk( task );
      struct param* param = script->params;
      while ( param ) {
         param->index = alloc_scalar( task );
         param = param->next;
      }
      do_node( task, script->body );
      t_add_opc( task, PCD_TERMINATE );
      script->size = task->block_walk->size_high;
      pop_block_walk( task );
      list_next( &i );
   }
   // Functions.
   list_iter_init( &i, &task->library_main->funcs );
   while ( ! list_end( &i ) ) {
      struct func* func = list_data( &i );   
      struct func_user* impl = func->impl;
      impl->obj_pos = t_tell( task );
      add_block_walk( task );
      struct param* param = func->params;
      while ( param ) {
         param->index = alloc_scalar( task );
         param = param->next;
      }
      do_default_params( task, func );
      list_iter_t k;
      list_iter_init( &k, &impl->body->stmts );
      while ( ! list_end( &k ) ) {
         do_node( task, list_data( &k ) );
         list_next( &k );
      }
      t_add_opc( task, PCD_RETURNVOID );
      impl->size = task->block_walk->size_high - func->max_param;
      pop_block_walk( task );
      list_next( &i );
   }
   // When utilizing the Little-E format, where instructions can be of
   // different size, add padding so any following data starts at an offset
   // that is multiple of 4.
   if ( task->library_main->format == FORMAT_LITTLE_E ) {
      int i = alignpad( t_tell( task ), 4 );
      while ( i ) {
         t_add_opc( task, PCD_TERMINATE );
         --i;
      }
   }
}

void do_block( struct task* task, struct block* block ) {
   add_block_walk( task );
   list_iter_t i;
   list_iter_init( &i, &block->stmts );
   while ( ! list_end( &i ) ) {
      do_node( task, list_data( &i ) );
      list_next( &i );
   }
   pop_block_walk( task );
}

void add_block_walk( struct task* task ) {
   struct block_walk* walk;
   if ( task->block_walk_free ) {
      walk = task->block_walk_free;
      task->block_walk_free = walk->prev;
   }
   else {
      walk = mem_alloc( sizeof( *walk ) );
   }
   walk->prev = task->block_walk;
   task->block_walk = walk;
   walk->format_block_usage = NULL;
   walk->size = 0;
   walk->size_high = 0;
   if ( walk->prev ) {
      walk->size = walk->prev->size;
      walk->size_high = walk->size;
   }
}

void pop_block_walk( struct task* task ) {
   struct block_walk* prev = task->block_walk->prev;
   if ( prev && task->block_walk->size_high > prev->size_high ) {
      prev->size_high = task->block_walk->size_high;
   }
   task->block_walk->prev = task->block_walk_free;
   task->block_walk_free = task->block_walk;
   task->block_walk = prev;
}

int alloc_scalar( struct task* task ) {
   int index = task->block_walk->size;
   ++task->block_walk->size;
   if ( task->block_walk->size > task->block_walk->size_high ) {
      task->block_walk->size_high = task->block_walk->size;
   }
   return index;
}

void dealloc_last_scalar( struct task* task ) {
   --task->block_walk->size;
}

void do_var( struct task* task, struct var* var ) {
   if ( var->storage != STORAGE_MAP ) {
      if ( var->storage == STORAGE_LOCAL ) {
         var->index = alloc_scalar( task );
      }
      if ( var->initial ) {
         if ( var->initial->multi ) {
            if ( var->storage == STORAGE_WORLD ||
               var->storage == STORAGE_GLOBAL ) {
               do_world_global_init( task, var );
            }
         }
         else {
            struct value* value = ( struct value* ) var->initial;
            push_expr( task, value->expr, false );
            update_indexed( task, var->storage, var->index, AOP_NONE );
         }
      }
   }
}

void do_world_global_init( struct task* task, struct var* var ) {
   // Nullify array.
   t_add_opc( task, PCD_PUSHNUMBER );
   t_add_arg( task, var->size - 1 );
   int loop = t_tell( task );
   t_add_opc( task, PCD_CASEGOTO );
   t_add_arg( task, 0 );
   t_add_arg( task, 0 );
   t_add_opc( task, PCD_DUP );
   t_add_opc( task, PCD_PUSHNUMBER );
   t_add_arg( task, 0 );
   update_element( task, var->storage, var->index, AOP_NONE );
   t_add_opc( task, PCD_PUSHNUMBER );
   t_add_arg( task, 1 );
   t_add_opc( task, PCD_SUBTRACT );
   t_add_opc( task, PCD_GOTO );
   t_add_arg( task, loop );
   int done = t_tell( task );
   t_seek( task, loop );
   t_add_opc( task, PCD_CASEGOTO );
   t_add_arg( task, -1 );
   t_add_arg( task, done );
   t_seek( task, OBJ_SEEK_END );
   // Assign elements.
   struct value* value = var->value;
   while ( value ) {
      t_add_opc( task, PCD_PUSHNUMBER );
      t_add_arg( task, value->index );
      t_add_opc( task, PCD_PUSHNUMBER );
      t_add_arg( task, value->expr->value );
      if ( value->expr->has_str && task->library_main->importable ) {
         t_add_opc( task, PCD_TAGSTRING );
      }
      update_element( task, var->storage, var->index, AOP_NONE );
      value = value->next;
   }
}

void do_node( struct task* task, struct node* node ) {
   switch ( node->type ) {
   case NODE_BLOCK:
      do_block( task, ( struct block* ) node );
      break;
   case NODE_SCRIPT_JUMP:
      do_script_jump( task, ( struct script_jump* ) node );
      break;
   case NODE_GOTO_LABEL:
      do_label( task, ( struct label* ) node );
      break;
   case NODE_GOTO:
      do_goto( task, ( struct goto_stmt* ) node );
      break;
   case NODE_PACKED_EXPR:
      do_expr_stmt( task, ( struct packed_expr* ) node );
      break;
   case NODE_VAR:
      do_var( task, ( struct var* ) node );
      break;
   case NODE_IF:
      do_if( task, ( struct if_stmt* ) node );
      break;
   case NODE_SWITCH:
      do_switch( task, ( struct switch_stmt* ) node );
      break;
   case NODE_CASE:
   case NODE_CASE_DEFAULT:
      do_case( task, ( struct case_label* ) node );
      break;
   case NODE_WHILE:
      do_while( task, ( struct while_stmt* ) node );
      break;
   case NODE_FOR:
      do_for( task, ( struct for_stmt* ) node );
      break;
   case NODE_JUMP:
      do_jump( task, ( struct jump* ) node );
      break;
   case NODE_RETURN:
      do_return( task, ( struct return_stmt* ) node );
      break;
   case NODE_FORMAT_ITEM:
      do_format_item( task, ( struct format_item* ) node );
      break;
   case NODE_PALTRANS:
      do_paltrans( task, ( struct paltrans* ) node );
      break;
   default:
      break;
   }
}

void do_script_jump( struct task* task, struct script_jump* stmt ) {
   if ( stmt->type == SCRIPT_JUMP_SUSPEND ) {
      t_add_opc( task, PCD_SUSPEND );
   }
   else if ( stmt->type == SCRIPT_JUMP_RESTART ) {
      t_add_opc( task, PCD_RESTART );
   }
   else {
      t_add_opc( task, PCD_TERMINATE );
   }
}

void do_label( struct task* task, struct label* label ) {
   label->obj_pos = t_tell( task );
   struct goto_stmt* stmt = label->stmts;
   while ( stmt ) {
      if ( stmt->obj_pos ) {
         t_seek( task, stmt->obj_pos );
         t_add_opc( task, PCD_GOTO );
         t_add_arg( task, label->obj_pos );
      }
      stmt = stmt->next;
   }
   list_iter_t i;
   list_iter_init( &i, &label->users );
   while ( ! list_end( &i ) ) {
      struct goto_stmt* stmt = list_data( &i );
      if ( stmt->obj_pos ) {
         t_seek( task, stmt->obj_pos );
         t_add_opc( task, PCD_GOTO );
         t_add_arg( task, label->obj_pos );
      }
      list_next( &i );
   }
   t_seek( task, OBJ_SEEK_END );
}

void do_goto( struct task* task, struct goto_stmt* stmt ) {
   if ( stmt->label->obj_pos ) {
      t_add_opc( task, PCD_GOTO );
      t_add_arg( task, stmt->label->obj_pos );
   }
   else {
      stmt->obj_pos = t_tell( task );
      t_add_opc( task, PCD_GOTO );
      t_add_arg( task, 0 );
   }
}

void do_expr( struct task* task, struct expr* expr ) {
   struct operand operand;
   init_operand( &operand );
   do_operand( task, &operand, expr->root );
   if ( operand.pushed ) {
      t_add_opc( task, PCD_DROP );
   }
}

void do_expr_stmt( struct task* task, struct packed_expr* stmt ) {
   do_expr( task, stmt->expr );
}

void init_operand( struct operand* operand ) {
   operand->type = NULL;
   operand->dim = NULL;
   operand->action = ACTION_PUSH_VALUE;
   operand->method = METHOD_NONE;
   operand->trigger = TRIGGER_INDEX;
   operand->storage = 0;
   operand->index = 0;
   operand->base = 0;
   operand->push = false;
   operand->push_temp = false;
   operand->pushed = false;
   operand->pushed_element = false;
}

void push_expr( struct task* task, struct expr* expr, bool temp ) {
   struct operand operand;
   init_operand( &operand );
   operand.push = true;
   operand.push_temp = temp;
   do_operand( task, &operand, expr->root );
}

void do_operand( struct task* task, struct operand* operand,
   struct node* node ) {
   if ( node->type == NODE_NAME_USAGE ) {
      struct name_usage* usage = ( struct name_usage* ) node;
      node = usage->object;
      if ( node->type == NODE_ALIAS ) {
         struct alias* alias = ( struct alias* ) node;
         node = &alias->target->node;
      }
   }
   if ( node->type == NODE_LITERAL ) {
      struct literal* literal = ( struct literal* ) node;
      t_add_opc( task, PCD_PUSHNUMBER );
      t_add_arg( task, literal->value );
      operand->pushed = true;
   }
   else if ( node->type == NODE_INDEXED_STRING_USAGE ) {
      struct indexed_string_usage* usage =
         ( struct indexed_string_usage* ) node;
      t_add_opc( task, PCD_PUSHNUMBER );
      t_add_arg( task, usage->string->index );
      // Strings in a library need to be tagged.
      if ( task->library_main->importable ) {
         t_add_opc( task, PCD_TAGSTRING );
      }
      operand->pushed = true;
   }
   else if ( node->type == NODE_BOOLEAN ) {
      struct boolean* boolean = ( struct boolean* ) node;
      t_add_opc( task, PCD_PUSHNUMBER );
      t_add_arg( task, boolean->value );
   }
   else if ( node->type == NODE_CONSTANT ) {
      do_constant( task, operand, ( struct constant* ) node );
   }
   else if ( node->type == NODE_VAR ) {
      do_var_name( task, operand, ( struct var* ) node );
   }
   else if ( node->type == NODE_PARAM ) {
      struct param* param = ( struct param* ) node;
      if ( operand->action == ACTION_PUSH_VALUE ) {
         push_indexed( task, STORAGE_LOCAL, param->index );
         operand->pushed = true;
      }
      else {
         operand->storage = STORAGE_LOCAL;
         operand->index = param->index;
      }
   }
   else if ( node->type == NODE_UNARY ) {
      do_unary( task, operand, ( struct unary* ) node );
   }
   else if ( node->type == NODE_SUBSCRIPT || node->type == NODE_ACCESS ) {
      do_object( task, operand, node );
   }
   else if ( node->type == NODE_CALL ) {
      do_call( task, operand, ( struct call* ) node );
   }
   else if ( node->type == NODE_BINARY ) {
      do_binary( task, operand, ( struct binary* ) node );
   }
   else if ( node->type == NODE_ASSIGN ) {
      do_assign( task, operand, ( struct assign* ) node );
   }
   else if ( node->type == NODE_PAREN ) {
      struct paren* paren = ( struct paren* ) node;
      do_operand( task, operand, paren->inside );
   }
   else if ( node->type == NODE_FUNC ) {
      struct func* func = ( struct func* ) node;
      if ( func->type == FUNC_ASPEC ) {
         struct func_aspec* impl = func->impl;
         t_add_opc( task, PCD_PUSHNUMBER );
         t_add_arg( task, impl->id );
      }
   }
}

void do_constant( struct task* task, struct operand* operand,
   struct constant* constant ) {
   t_add_opc( task, PCD_PUSHNUMBER );
   t_add_arg( task, constant->value );
   if ( constant->expr && constant->expr->has_str &&
      task->library_main->importable ) {
      t_add_opc( task, PCD_TAGSTRING );
   }
   operand->pushed = true;
}

void do_unary( struct task* task, struct operand* operand,
   struct unary* unary ) {
   if ( unary->op == UOP_PRE_INC || unary->op == UOP_PRE_DEC ) {
      do_pre_inc( task, operand, unary );
   }
   else if ( unary->op == UOP_POST_INC || unary->op == UOP_POST_DEC ) {
      do_post_inc( task, operand, unary );
   }
   else {
      struct operand target;
      init_operand( &target );
      target.push = true;
      do_operand( task, &target, unary->operand );
      int code = PCD_NONE;
      switch ( unary->op ) {
      case UOP_MINUS:
         code = PCD_UNARYMINUS;
         break;
      // Unary plus is ignored.
      case UOP_LOG_NOT:
         code = PCD_NEGATELOGICAL;
         break;
      case UOP_BIT_NOT:
         code = PCD_NEGATEBINARY;
         break;
      default:
         break;
      }
      if ( code ) {
         t_add_opc( task, code );
      }
      operand->pushed = true;
   }
}

void do_pre_inc( struct task* task, struct operand* operand,
   struct unary* unary ) {
   struct operand object;
   init_operand( &object );
   object.action = ACTION_PUSH_VAR;
   do_operand( task, &object, unary->operand );
   if ( object.method == METHOD_ELEMENT ) {
      if ( operand->push ) {
         t_add_opc( task, PCD_DUP );
      }
      if ( unary->op == UOP_PRE_INC ) {
         inc_element( task, object.storage, object.index );
      }
      else {
         dec_element( task, object.storage, object.index );
      }
      if ( operand->push ) {
         push_element( task, object.storage, object.index );
         operand->pushed = true;
      }
   }
   else {
      if ( unary->op == UOP_PRE_INC ) {
         inc_indexed( task, object.storage, object.index );
      }
      else {
         dec_indexed( task, object.storage, object.index );
      }
      if ( operand->push ) {
         push_indexed( task, object.storage, object.index );
         operand->pushed = true;
      }
   }
}

void do_post_inc( struct task* task, struct operand* operand,
   struct unary* unary ) {
   struct operand object;
   init_operand( &object );
   object.action = ACTION_PUSH_VAR;
   do_operand( task, &object, unary->operand );
   if ( object.method == METHOD_ELEMENT ) {
      if ( operand->push ) {
         t_add_opc( task, PCD_DUP );
         push_element( task, object.storage, object.index );
         t_add_opc( task, PCD_SWAP );
         operand->pushed = true;
      }
      if ( unary->op == UOP_POST_INC ) {
         inc_element( task, object.storage, object.index );
      }
      else {
         dec_element( task, object.storage, object.index );
      }
   }
   else {
      if ( operand->push ) {
         push_indexed( task, object.storage, object.index );
         operand->pushed = true;
      }
      if ( unary->op == UOP_POST_INC ) {
         inc_indexed( task, object.storage, object.index );
      }
      else {
         dec_indexed( task, object.storage, object.index );
      }
   }
}

void do_call( struct task* task, struct operand* operand, struct call* call ) {
   struct func* func = call->func;
   if ( func->type == FUNC_ASPEC ) {
      int num = 0;
      list_iter_t i;
      list_iter_init( &i, &call->args );
      while ( ! list_end( &i ) ) {
         push_expr( task, list_data( &i ), false );
         list_next( &i );
         ++num;
      }
      struct func_aspec* aspec = func->impl;
      if ( operand->push ) {
         while ( num < 5 ) {
            t_add_opc( task, PCD_PUSHNUMBER );
            t_add_arg( task, 0 );
            ++num;
         }
         t_add_opc( task, PCD_LSPEC5RESULT );
         t_add_arg( task, aspec->id );
      }
      else if ( num ) {
         t_add_opc( task, g_aspec_code[ num - 1 ] );
         t_add_arg( task, aspec->id );
      }
      else {
         t_add_opc( task, PCD_PUSHNUMBER );
         t_add_arg( task, 0 );
         t_add_opc( task, PCD_LSPEC1 );
         t_add_arg( task, aspec->id );
      }
   }
   else if ( func->type == FUNC_EXT ) {
      list_iter_t i;
      list_iter_init( &i, &call->args );
      struct param* param = func->params;
      while ( ! list_end( &i ) ) {
         struct expr* expr = list_data( &i );
         struct operand arg;
         init_operand( &arg );
         arg.push = true;
         do_operand( task, &arg, expr->root );
         list_next( &i );
         param = param->next;
      }
      int count = list_size( &call->args );
      int skipped = 0;
      while ( param ) {
         if ( param->expr->folded && ! param->expr->value ) {
            ++skipped;
         }
         else {
            count += skipped;
            while ( skipped ) {
               t_add_opc( task, PCD_PUSHNUMBER );
               t_add_arg( task, 0 );
               --skipped;
            }
            struct operand arg;
            init_operand( &arg );
            arg.push = true;
            do_operand( task, &arg, param->expr->root );
            ++count;
         }
         param = param->next;
      }
      struct func_ext* impl = func->impl;
      t_add_opc( task, PCD_CALLFUNC );
      t_add_arg( task, count );
      t_add_arg( task, impl->id );
      if ( func->value ) {
         operand->pushed = true;
      }
   }
   else if ( func->type == FUNC_DED ) {
      struct func_ded* ded = func->impl;
      list_iter_t i;
      list_iter_init( &i, &call->args );
      struct param* param = func->params;
      while ( ! list_end( &i ) ) {
         push_expr( task, list_data( &i ), false );
         list_next( &i );
         param = param->next;
      }
      // Default arguments.
      while ( param ) {
         struct operand arg;
         init_operand( &arg );
         arg.push = true;
         do_operand( task, &arg, param->expr->root );
         param = param->next;
      }
      t_add_opc( task, ded->opcode );
      if ( func->value ) {
         operand->pushed = true;
      }
   }
   else if ( func->type == FUNC_FORMAT ) {
      do_format_call( task, call, func );
   }
   else if ( func->type == FUNC_USER ) {
      list_iter_t i;
      list_iter_init( &i, &call->args );
      struct param* param = func->params;
      while ( ! list_end( &i ) ) {
         push_expr( task, list_data( &i ), false );
         list_next( &i );
         param = param->next;
      }
      // Default arguments.
      while ( param ) {
         t_add_opc( task, PCD_PUSHNUMBER );
         t_add_arg( task, 0 );
         param = param->next;
      }
      // Number of real arguments passed, for a function with default
      // parameters.
      if ( func->min_param != func->max_param ) {
         t_add_opc( task, PCD_PUSHNUMBER );
         t_add_arg( task, list_size( &call->args ) );
      }
      struct func_user* impl = func->impl;
      if ( func->value ) {
         t_add_opc( task, PCD_CALL );
         t_add_arg( task, impl->index );
         operand->pushed = true;
      }
      else {
         t_add_opc( task, PCD_CALLDISCARD );
         t_add_arg( task, impl->index );
      }
   }
   else if ( func->type == FUNC_INTERNAL ) {
      struct func_intern* impl = func->impl; 
      if ( impl->id == INTERN_FUNC_ACS_EXECWAIT ) {
         list_iter_t i;
         list_iter_init( &i, &call->args );
         push_expr( task, list_data( &i ), false );
         t_add_opc( task, PCD_DUP );
         list_next( &i );
         // Second argument unused.
         list_next( &i );
         // Second argument to Acs_Execute is 0--the current map.
         t_add_opc( task, PCD_PUSHNUMBER );
         t_add_arg( task, 0 );
         while ( ! list_end( &i ) ) {
            push_expr( task, list_data( &i ), true );
            list_next( &i );
         }
         t_add_opc( task, g_aspec_code[ list_size( &call->args ) - 1 ] );
         t_add_arg( task, 80 );
         t_add_opc( task, PCD_SCRIPTWAIT );
      }
      else if ( impl->id == INTERN_FUNC_STR_LENGTH ) {
         do_operand( task, operand, call->operand );
         t_add_opc( task, PCD_STRLEN );
      }
      else if ( impl->id == INTERN_FUNC_STR_AT ) {
         do_operand( task, operand, call->operand );
         push_expr( task, list_head( &call->args ), true );
         t_add_opc( task, PCD_CALLFUNC );
         t_add_arg( task, 2 );
         t_add_arg( task, 15 );
      }
   }
}

void do_format_call( struct task* task, struct call* call,
   struct func* func ) {
   t_add_opc( task, PCD_BEGINPRINT );
   list_iter_t i;
   list_iter_init( &i, &call->args );
   struct node* node = list_data( &i );
   // Format-block:
   if ( node->type == NODE_FORMAT_BLOCK_USAGE ) {
      struct format_block_usage* usage =
         ( struct format_block_usage* ) node;
      // When a format block is used more than once in the same expression,
      // instead of duplicating the code, use a goto instruction to enter the
      // format block. At each usage except the last, before the format block
      // is used, a unique number is pushed. This number is used to determine
      // the return location. 
      if ( usage->next ) {
         usage->obj_pos = t_tell( task );
         t_add_opc( task, PCD_PUSHNUMBER );
         t_add_arg( task, 0 );
         t_add_opc( task, PCD_GOTO );
         t_add_arg( task, 0 );
         if ( ! task->block_walk->format_block_usage ) {
            task->block_walk->format_block_usage = usage;
         }
      }
      else {
         int block_pos = t_tell( task );
         do_block( task, usage->block );
         usage = task->block_walk->format_block_usage;
         if ( usage ) {
            // Update block jumps.
            int count = 0;
            while ( usage->next ) {
               t_seek( task, usage->obj_pos );
               t_add_opc( task, PCD_PUSHNUMBER );
               t_add_arg( task, count );
               t_add_opc( task, PCD_GOTO );
               t_add_arg( task, block_pos );
               usage->obj_pos = t_tell( task );
               usage = usage->next;
               ++count;
            }
            // Publish return jumps. A sorted-case-goto can be used here, but a
            // case-goto will suffice for now.
            t_seek( task, OBJ_SEEK_END );
            usage = task->block_walk->format_block_usage;
            count = 0;
            while ( usage->next ) {
               t_add_opc( task, PCD_CASEGOTO );
               t_add_arg( task, count );
               t_add_arg( task, usage->obj_pos );
               usage = usage->next;
               ++count;
            }
            t_add_opc( task, PCD_DROP );
            task->block_walk->format_block_usage = NULL;
         }
      }
      list_next( &i );
   }
   // Format-list:
   else {
      do_format_item( task, list_data( &i ) );
      list_next( &i );
   }
   // Other arguments.
   if ( func->max_param > 1 ) {
      t_add_opc( task, PCD_MOREHUDMESSAGE );
      int param = 1;
      while ( ! list_end( &i ) ) {
         if ( param == func->min_param ) {
            t_add_opc( task, PCD_OPTHUDMESSAGE );
         }
         push_expr( task, list_data( &i ), false );
         ++param;
         list_next( &i );
      }
   }
   struct func_format* format = func->impl;
   t_add_opc( task, format->opcode );
} 

void do_format_item( struct task* task, struct format_item* item ) {
   while ( item ) {
      if ( item->cast == FCAST_ARRAY ) {
         struct operand object;
         init_operand( &object );
         object.action = ACTION_PUSH_VAR;
         do_operand( task, &object, item->expr->root );
         t_add_opc( task, PCD_PUSHNUMBER );
         t_add_arg( task, object.index );
         int code = PCD_PRINTMAPCHARARRAY;
         switch ( object.storage ) {
         case STORAGE_WORLD:
            code = PCD_PRINTWORLDCHARARRAY;
            break;
         case STORAGE_GLOBAL:
            code = PCD_PRINTGLOBALCHARARRAY;
            break;
         default:
            break;
         }
         t_add_opc( task, code );
      }
      else {
         static const int casts[] = {
            PCD_PRINTBINARY,
            PCD_PRINTCHARACTER,
            PCD_PRINTNUMBER,
            PCD_PRINTFIXED,
            PCD_PRINTBIND,
            PCD_PRINTLOCALIZED,
            PCD_PRINTNAME,
            PCD_PRINTSTRING,
            PCD_PRINTHEX };
         STATIC_ASSERT( FCAST_TOTAL == 10 );
         push_expr( task, item->expr, false );
         t_add_opc( task, casts[ item->cast - 1 ] );
      }
      item = item->next;
   }
}

void do_binary( struct task* task, struct operand* operand,
   struct binary* binary ) {
   // Logical-or and logical-and both perform shortcircuit evaluation.
   if ( binary->op == BOP_LOG_OR ) {
      struct operand lside;
      init_operand( &lside );
      lside.push = true;
      lside.push_temp = true;
      do_operand( task, &lside, binary->lside );
      int test = t_tell( task );
      t_add_opc( task, PCD_IFNOTGOTO );
      t_add_arg( task, 0 );
      t_add_opc( task, PCD_PUSHNUMBER );
      t_add_arg( task, 1 );
      int jump = t_tell( task );
      t_add_opc( task, PCD_GOTO );
      t_add_arg( task, 0 );
      struct operand rside;
      init_operand( &rside );
      rside.push = true;
      rside.push_temp = true;
      int next = t_tell( task );
      do_operand( task, &rside, binary->rside );
      // Optimization: When doing a calculation temporarily, there's no need to
      // convert the second operand to a 0 or 1. Just use the operand directly.
      if ( ! operand->push_temp ) {
         t_add_opc( task, PCD_NEGATELOGICAL );
         t_add_opc( task, PCD_NEGATELOGICAL );
      }
      int done = t_tell( task );
      t_seek( task, test );
      t_add_opc( task, PCD_IFNOTGOTO );
      t_add_arg( task, next );
      t_seek( task, jump );
      t_add_opc( task, PCD_GOTO );
      t_add_arg( task, done );
      t_seek( task, OBJ_SEEK_END );
      operand->pushed = true;
   }
   else if ( binary->op == BOP_LOG_AND ) {
      struct operand lside;
      init_operand( &lside );
      lside.push = true;
      lside.push_temp = true;
      do_operand( task, &lside, binary->lside );
      int test = t_tell( task );
      t_add_opc( task, PCD_IFGOTO );
      t_add_arg( task, 0 );
      t_add_opc( task, PCD_PUSHNUMBER );
      t_add_arg( task, 0 );
      int jump = t_tell( task );
      t_add_opc( task, PCD_GOTO );
      t_add_arg( task, 0 );
      struct operand rside;
      init_operand( &rside );
      rside.push = true;
      rside.push_temp = true;
      int next = t_tell( task );
      do_operand( task, &rside, binary->rside );
      if ( ! operand->push_temp ) {
         t_add_opc( task, PCD_NEGATELOGICAL );
         t_add_opc( task, PCD_NEGATELOGICAL );
      }
      int done = t_tell( task );
      t_seek( task, test );
      t_add_opc( task, PCD_IFGOTO );
      t_add_arg( task, next );
      t_seek( task, jump );
      t_add_opc( task, PCD_GOTO );
      t_add_arg( task, done );
      t_seek( task, OBJ_SEEK_END );
      operand->pushed = true;
   }
   else {
      struct operand lside;
      init_operand( &lside );
      lside.push = true;
      do_operand( task, &lside, binary->lside );
      struct operand rside;
      init_operand( &rside );
      rside.push = true;
      do_operand( task, &rside, binary->rside );
      int code = PCD_NONE;
      switch ( binary->op ) {
      case BOP_BIT_OR: code = PCD_ORBITWISE; break;
      case BOP_BIT_XOR: code = PCD_EORBITWISE; break;
      case BOP_BIT_AND: code = PCD_ANDBITWISE; break;
      case BOP_EQ: code = PCD_EQ; break;
      case BOP_NEQ: code = PCD_NE; break;
      case BOP_LT: code = PCD_LT; break;
      case BOP_LTE: code = PCD_LE; break;
      case BOP_GT: code = PCD_GT; break;
      case BOP_GTE: code = PCD_GE; break;
      case BOP_SHIFT_L: code = PCD_LSHIFT; break;
      case BOP_SHIFT_R: code = PCD_RSHIFT; break;
      case BOP_ADD: code = PCD_ADD; break;
      case BOP_SUB: code = PCD_SUBTRACT; break;
      case BOP_MUL: code = PCD_MULIPLY; break;
      case BOP_DIV: code = PCD_DIVIDE; break;
      case BOP_MOD: code = PCD_MODULUS; break;
      default: break;
      }
      t_add_opc( task, code );
      operand->pushed = true;
   }
}

void set_var( struct task* task, struct operand* operand, struct var* var ) {
   operand->type = var->type;
   operand->dim = var->dim;
   operand->storage = var->storage;
   if ( ! var->type->primitive || var->dim ) {
      operand->method = METHOD_ELEMENT;
      operand->index = var->index;
   }
   else {
      operand->method = METHOD_INDEXED;
      operand->index = var->index;
   }
}

void do_var_name( struct task* task, struct operand* operand,
   struct var* var ) {
   set_var( task, operand, var );
   // For element-based variables, an index marking the start of the variable
   // data needs to be on the stack.
   if ( operand->method == METHOD_ELEMENT ) {
      t_add_opc( task, PCD_PUSHNUMBER );
      t_add_arg( task, operand->base );
   }
   else {
      if ( operand->action == ACTION_PUSH_VALUE ) {
         push_indexed( task, operand->storage, operand->index );
         operand->pushed = true;
      }
   }
}

void do_object( struct task* task, struct operand* operand,
   struct node* node ) {
   if ( node->type == NODE_ACCESS ) {
      do_access( task, operand, ( struct access* ) node );
   }
   else if ( node->type == NODE_SUBSCRIPT ) {
      do_subscript( task, operand, ( struct subscript* ) node );
   }
   if ( operand->method == METHOD_ELEMENT ) {
      if ( operand->pushed_element ) {
         if ( operand->base ) {
            t_add_opc( task, PCD_PUSHNUMBER );
            t_add_arg( task, operand->base );
            t_add_opc( task, PCD_ADD );
         }
      }
      else {
         t_add_opc( task, PCD_PUSHNUMBER );
         t_add_arg( task, operand->base );
      }
   }
   if ( operand->action == ACTION_PUSH_VALUE &&
      operand->method != METHOD_NONE ) {
      if ( operand->method == METHOD_ELEMENT ) {
         push_element( task, operand->storage, operand->index );
      }
      else {
         push_indexed( task, operand->storage, operand->index );
      }
      operand->pushed = true;
   }
}

void do_subscript( struct task* task, struct operand* operand,
   struct subscript* subscript ) {
   struct node* lside = subscript->lside;
   while ( lside->type == NODE_PAREN ) {
      struct paren* paren = ( struct paren* ) lside;
      lside = paren->inside;
   }
   if ( lside->type == NODE_NAME_USAGE ) {
      struct name_usage* usage = ( struct name_usage* ) lside;
      lside = usage->object;
      if ( lside->type == NODE_ALIAS ) {
         struct alias* alias = ( struct alias* ) lside;
         lside = &alias->target->node;
      }
   }
   // Left side:
   if ( lside->type == NODE_VAR ) {
      set_var( task, operand, ( struct var* ) lside );
   }
   else if ( lside->type == NODE_ACCESS ) {
      do_access( task, operand, ( struct access* ) lside );
   }
   else if ( lside->type == NODE_SUBSCRIPT ) {
      do_subscript( task, operand, ( struct subscript* ) lside );
   }
   // Dimension:
   struct operand index;
   init_operand( &index );
   index.push = true;
   index.push_temp = true;
   do_operand( task, &index, subscript->index->root );
   if ( operand->dim->next ) {
      t_add_opc( task, PCD_PUSHNUMBER );
      t_add_arg( task, operand->dim->element_size );
      t_add_opc( task, PCD_MULIPLY );
   }
   else if ( ! operand->type->primitive ) {
      t_add_opc( task, PCD_PUSHNUMBER );
      t_add_arg( task, operand->type->size );
      t_add_opc( task, PCD_MULIPLY );
   }
   if ( operand->pushed_element ) {
      t_add_opc( task, PCD_ADD );
   }
   else {
      operand->pushed_element = true;
   }
   operand->dim = operand->dim->next;
}

void do_access( struct task* task, struct operand* operand,
   struct access* access ) {
   struct node* lside = access->lside;
   struct node* rside = access->rside;
   while ( lside->type == NODE_PAREN ) {
      struct paren* paren = ( struct paren* ) lside;
      lside = paren->inside;
   }
   if ( lside->type == NODE_NAME_USAGE ) {
      struct name_usage* usage = ( struct name_usage* ) lside;
      lside = usage->object;
      if ( lside->type == NODE_ALIAS ) {
         struct alias* alias = ( struct alias* ) lside;
         lside = &alias->target->node;
      }
   }
   // See if the left side is a namespace.
   struct node* object = lside;
   if ( object->type == NODE_ACCESS ) {
      struct access* nested = ( struct access* ) object;
      object = nested->rside;
      if ( object->type == NODE_ALIAS ) {
         struct alias* alias = ( struct alias* ) object;
         object = &alias->target->node;
      }
   }
   // When the left side is a module, only process the right side.
   if ( object->type == NODE_REGION || object->type == NODE_REGION_HOST ||
      object->type == NODE_REGION_UPMOST ) {
      lside = access->rside;
      if ( lside->type == NODE_ALIAS ) {
         struct alias* alias = ( struct alias* ) lside;
         lside = &alias->target->node;
      }
      rside = NULL;
   }
   // Left side:
   if ( lside->type == NODE_VAR ) {
      set_var( task, operand, ( struct var* ) lside );
   }
   else if ( lside->type == NODE_CONSTANT ) {
      do_constant( task, operand, ( struct constant* ) lside );
   }
   else if ( lside->type == NODE_ACCESS ) {
      do_access( task, operand, ( struct access* ) lside );
   }
   else if ( lside->type == NODE_SUBSCRIPT ) {
      do_subscript( task, operand, ( struct subscript* ) lside );
   }
   else {
      do_operand( task, operand, lside );
   }
   // Right side:
   if ( rside && rside->type == NODE_TYPE_MEMBER ) {
      struct type_member* member = ( struct type_member* ) rside;
      t_add_opc( task, PCD_PUSHNUMBER );
      t_add_arg( task, member->offset );
      if ( operand->pushed_element ) {
         t_add_opc( task, PCD_ADD );
      }
      else {
         operand->pushed_element = true;
      }
      operand->type = member->type;
      operand->dim = member->dim;
   }
}

void do_assign( struct task* task, struct operand* operand,
   struct assign* assign ) {
   struct operand lside;
   init_operand( &lside );
   lside.action = ACTION_PUSH_VAR;
   do_operand( task, &lside, assign->lside );
   if ( lside.method == METHOD_ELEMENT ) {
      if ( operand->push ) {
         t_add_opc( task, PCD_DUP );
      }
      struct operand rside;
      init_operand( &rside );
      rside.push = true;
      do_operand( task, &rside, assign->rside );
      update_element( task, lside.storage, lside.index, assign->op );
      if ( operand->push ) {
         push_element( task, lside.storage, lside.index );
         operand->pushed = true;
      }
   }
   else {
      struct operand rside;
      init_operand( &rside );
      rside.push = true;
      do_operand( task, &rside, assign->rside );
      if ( assign->op == AOP_NONE && operand->push ) {
         t_add_opc( task, PCD_DUP );
         operand->pushed = true;
      }
      update_indexed( task, lside.storage, lside.index, assign->op );
      if ( assign->op != AOP_NONE && operand->push ) {
         push_indexed( task, lside.storage, lside.index );
         operand->pushed = true;
      }
   }
}

void push_indexed( struct task* task, int storage, int index ) {
   int code = PCD_PUSHSCRIPTVAR;
   switch ( storage ) {
   case STORAGE_MAP:
      code = PCD_PUSHMAPVAR;
      break;
   case STORAGE_WORLD:
      code = PCD_PUSHWORLDVAR;
      break;
   case STORAGE_GLOBAL:
      code = PCD_PUSHGLOBALVAR;
      break;
   default:
      break;
   }
   t_add_opc( task, code );
   t_add_arg( task, index );
}

void push_element( struct task* task, int storage, int index ) {
   int code = PCD_PUSHMAPARRAY;
   switch ( storage ) {
   case STORAGE_WORLD:
      code = PCD_PUSHWORLDARRAY;
      break;
   case STORAGE_GLOBAL:
      code = PCD_PUSHGLOBALARRAY;
      break;
   default:
      break;
   }
   t_add_opc( task, code );
   t_add_arg( task, index );
}

void update_indexed( struct task* task, int storage, int index, int op ) {
   static const int code[] = {
      PCD_ASSIGNSCRIPTVAR, PCD_ASSIGNMAPVAR, PCD_ASSIGNWORLDVAR,
         PCD_ASSIGNGLOBALVAR,
      PCD_ADDSCRIPTVAR, PCD_ADDMAPVAR, PCD_ADDWORLDVAR, PCD_ADDGLOBALVAR,
      PCD_SUBSCRIPTVAR, PCD_SUBMAPVAR, PCD_SUBWORLDVAR, PCD_SUBGLOBALVAR,
      PCD_MULSCRIPTVAR, PCD_MULMAPVAR, PCD_MULWORLDVAR, PCD_MULGLOBALVAR,
      PCD_DIVSCRIPTVAR, PCD_DIVMAPVAR, PCD_DIVWORLDVAR, PCD_DIVGLOBALVAR,
      PCD_MODSCRIPTVAR, PCD_MODMAPVAR, PCD_MODWORLDVAR, PCD_MODGLOBALVAR,
      PCD_LSSCRIPTVAR, PCD_LSMAPVAR, PCD_LSWORLDVAR, PCD_LSGLOBALVAR,
      PCD_RSSCRIPTVAR, PCD_RSMAPVAR, PCD_RSWORLDVAR, PCD_RSGLOBALVAR,
      PCD_ANDSCRIPTVAR, PCD_ANDMAPVAR, PCD_ANDWORLDVAR, PCD_ANDGLOBALVAR,
      PCD_EORSCRIPTVAR, PCD_EORMAPVAR, PCD_EORWORLDVAR, PCD_EORGLOBALVAR,
      PCD_ORSCRIPTVAR, PCD_ORMAPVAR, PCD_ORWORLDVAR, PCD_ORGLOBALVAR };
   int pos = 0;
   switch ( storage ) {
   case STORAGE_MAP: pos = 1; break;
   case STORAGE_WORLD: pos = 2; break;
   case STORAGE_GLOBAL: pos = 3; break;
   default: break;
   }
   t_add_opc( task, code[ op * 4 + pos ] );
   t_add_arg( task, index );
}

void update_element( struct task* task, int storage, int index, int op ) {
   static const int code[] = {
      PCD_ASSIGNMAPARRAY, PCD_ASSIGNWORLDARRAY, PCD_ASSIGNGLOBALARRAY,
      PCD_ADDMAPARRAY, PCD_ADDWORLDARRAY, PCD_ADDGLOBALARRAY,
      PCD_SUBMAPARRAY, PCD_SUBWORLDARRAY, PCD_SUBGLOBALARRAY,
      PCD_MULMAPARRAY, PCD_MULWORLDARRAY, PCD_MULGLOBALARRAY,
      PCD_DIVMAPARRAY, PCD_DIVWORLDARRAY, PCD_DIVGLOBALARRAY,
      PCD_MODMAPARRAY, PCD_MODWORLDARRAY, PCD_MODGLOBALARRAY,
      PCD_LSMAPARRAY, PCD_LSWORLDARRAY, PCD_LSGLOBALARRAY,
      PCD_RSMAPARRAY, PCD_RSWORLDARRAY, PCD_RSGLOBALARRAY,
      PCD_ANDMAPARRAY, PCD_ANDWORLDARRAY, PCD_ANDGLOBALARRAY,
      PCD_EORMAPARRAY, PCD_EORWORLDARRAY, PCD_EORGLOBALARRAY,
      PCD_ORMAPARRAY, PCD_ORWORLDARRAY, PCD_ORGLOBALARRAY };
   int pos = 0;
   switch ( storage ) {
   case STORAGE_WORLD: pos = 1; break;
   case STORAGE_GLOBAL: pos = 2; break;
   default: break;
   }
   t_add_opc( task, code[ op * 3 + pos ] );
   t_add_arg( task, index );
}

void inc_indexed( struct task* task, int storage, int index ) {
   int code = PCD_INCSCRIPTVAR;
   switch ( storage ) {
   case STORAGE_MAP:
      code = PCD_INCMAPVAR;
      break;
   case STORAGE_WORLD:
      code = PCD_INCWORLDVAR;
      break;
   case STORAGE_GLOBAL:
      code = PCD_INCGLOBALVAR;
      break;
   default:
      break;
   }
   t_add_opc( task, code );
   t_add_arg( task, index );
}

void dec_indexed( struct task* task, int storage, int index ) {
   int code = PCD_DECSCRIPTVAR;
   switch ( storage ) {
   case STORAGE_MAP:
      code = PCD_DECMAPVAR;
      break;
   case STORAGE_WORLD:
      code = PCD_DECWORLDVAR;
      break;
   case STORAGE_GLOBAL:
      code = PCD_DECGLOBALVAR;
      break;
   default:
      break;
   }
   t_add_opc( task, code );
   t_add_arg( task, index );
}

void inc_element( struct task* task, int storage, int index ) {
   int code = PCD_INCMAPARRAY;
   switch ( storage ) {
   case STORAGE_WORLD:
      code = PCD_INCWORLDARRAY;
      break;
   case STORAGE_GLOBAL:
      code = PCD_INCGLOBALARRAY;
      break;
   default:
      break;
   }
   t_add_opc( task, code );
   t_add_arg( task, index );
}

void dec_element( struct task* task, int storage, int index ) {
   int code = PCD_DECMAPARRAY;
   switch ( storage ) {
   case STORAGE_WORLD:
      code = PCD_DECWORLDARRAY;
      break;
   case STORAGE_GLOBAL:
      code = PCD_DECGLOBALARRAY;
      break;
   default:
      break;
   }
   t_add_opc( task, code );
   t_add_arg( task, index );
}

void do_if( struct task* task, struct if_stmt* stmt ) {
   struct operand expr;
   init_operand( &expr );
   expr.push = true;
   expr.push_temp = true;
   do_operand( task, &expr, stmt->expr->root );
   int cond = t_tell( task );
   t_add_opc( task, PCD_IFNOTGOTO );
   t_add_arg( task, 0 );
   do_node( task, stmt->body );
   int bail = t_tell( task );
   if ( stmt->else_body ) {
      // Exit from if block:
      int bail_if_block = t_tell( task );
      t_add_opc( task, PCD_GOTO );
      t_add_arg( task, 0 ); 
      bail = t_tell( task );
      do_node( task, stmt->else_body );
      int stmt_end = t_tell( task );
      t_seek( task, bail_if_block );
      t_add_opc( task, PCD_GOTO );
      t_add_arg( task, stmt_end );
   }
   t_seek( task, cond );
   t_add_opc( task, PCD_IFNOTGOTO );
   t_add_arg( task, bail );
   t_seek( task, OBJ_SEEK_END );
}

void do_switch( struct task* task, struct switch_stmt* stmt ) {
   struct operand expr;
   init_operand( &expr );
   expr.push = true;
   expr.push_temp = true;
   do_operand( task, &expr, stmt->expr->root );
   int num_cases = 0;
   struct case_label* label = stmt->case_head;
   while ( label ) {
      ++num_cases;
      label = label->next;
   }
   int test = t_tell( task );
   if ( num_cases ) {
      t_add_opc( task, PCD_CASEGOTOSORTED );
      t_add_arg( task, 0 );
      for ( int i = 0; i < num_cases; ++i ) {
         t_add_arg( task, 0 );
         t_add_arg( task, 0 );
      }
   }
   t_add_opc( task, PCD_DROP );
   int fail = t_tell( task );
   t_add_opc( task, PCD_GOTO );
   t_add_arg( task, 0 );
   do_node( task, stmt->body );
   int done = t_tell( task );
   if ( num_cases ) {
      t_seek( task, test );
      t_add_opc( task, PCD_CASEGOTOSORTED );
      t_add_arg( task, num_cases );
      label = stmt->case_head;
      while ( label ) {
         t_add_arg( task, label->expr->value );
         t_add_arg( task, label->offset );
         label = label->next;
      }
   }
   t_seek( task, fail );
   t_add_opc( task, PCD_GOTO );
   int fail_pos = done;
   if ( stmt->case_default ) {
      fail_pos = stmt->case_default->offset;
   }
   t_add_arg( task, fail_pos );
   do_jump_target( task, stmt->jump_break, done );
}

void do_case( struct task* task, struct case_label* label ) {
   label->offset = t_tell( task );
}

void do_while( struct task* task, struct while_stmt* stmt ) {
   int test = 0;
   int done = 0;
   if ( stmt->type == WHILE_WHILE || stmt->type == WHILE_UNTIL ) {
      int jump = 0;
      if ( ! stmt->expr->folded || (
         ( stmt->type == WHILE_WHILE && ! stmt->expr->value ) ||
         ( stmt->type == WHILE_UNTIL && stmt->expr->value ) ) ) {
         jump = t_tell( task );
         t_add_opc( task, PCD_GOTO );
         t_add_arg( task, 0 );
      }
      int body = t_tell( task );
      do_node( task, stmt->body );
      if ( stmt->expr->folded ) {
         if ( ( stmt->type == WHILE_WHILE && stmt->expr->value ) ||
            ( stmt->type == WHILE_UNTIL && ! stmt->expr->value ) ) {
            t_add_opc( task, PCD_GOTO );
            t_add_arg( task, body );
            done = t_tell( task );
            test = body;
         }
         else {
            done = t_tell( task );
            test = done;
            t_seek( task, jump );
            t_add_opc( task, PCD_GOTO );
            t_add_arg( task, done );
         }
      }
      else {
         test = t_tell( task );
         struct operand expr;
         init_operand( &expr );
         expr.push = true;
         expr.push_temp = true;
         do_operand( task, &expr, stmt->expr->root );
         int code = PCD_IFGOTO;
         if ( stmt->type == WHILE_UNTIL ) {
            code = PCD_IFNOTGOTO;
         }
         t_add_opc( task, code );
         t_add_arg( task, body );
         done = t_tell( task );
         t_seek( task, jump );
         t_add_opc( task, PCD_GOTO );
         t_add_arg( task, test );
      }
   }
   // do-while / do-until.
   else {
      int body = t_tell( task );
      do_node( task, stmt->body );
      // Condition:
      if ( stmt->expr->folded ) {
         // Optimization: Only loop when the condition is satisfied.
         if ( ( stmt->type == WHILE_DO_WHILE && stmt->expr->value ) ||
            ( stmt->type == WHILE_DO_UNTIL && ! stmt->expr->value ) ) {
            t_add_opc( task, PCD_GOTO );
            t_add_arg( task, body );
            done = t_tell( task );
            test = body;
         }
         else {
            done = t_tell( task );
            test = done;
         }
      }
      else {
         test = t_tell( task );
         struct operand expr;
         init_operand( &expr );
         expr.push = true;
         expr.push_temp = true;
         do_operand( task, &expr, stmt->expr->root );
         int code = PCD_IFGOTO;
         if ( stmt->type == WHILE_DO_UNTIL ) {
            code = PCD_IFNOTGOTO;
         }
         t_add_opc( task, code );
         t_add_arg( task, body );
         done = t_tell( task );
      }
   }
   do_jump_target( task, stmt->jump_continue, test );
   do_jump_target( task, stmt->jump_break, done );
}

void do_for( struct task* task, struct for_stmt* stmt ) {
   if ( stmt->init ) {
      struct expr_link* link = stmt->init;
      while ( link ) {
         do_expr( task, link->expr );
         link = link->next;
      }
   }
   else if ( list_size( &stmt->vars ) ) {
      list_iter_t i;
      list_iter_init( &i, &stmt->vars );
      while ( ! list_end( &i ) ) {
         do_var( task, list_data( &i ) );
         list_next( &i );
      }
   }
   int jump = 0;
   if ( stmt->expr ) {
      // Optimization:
      if ( stmt->expr->folded ) {
         if ( ! stmt->expr->value ) {
            return;
         }
      }
      // Optimization:
      else {
         jump = t_tell( task );
         t_add_opc( task, PCD_GOTO );
         t_add_arg( task, 0 );
      }
   }
   int body = t_tell( task );
   do_node( task, stmt->body );
   int next = t_tell( task );
   if ( stmt->post ) {
      struct expr_link* link = stmt->post;
      while ( link ) {
         do_expr( task, link->expr );
         link = link->next;
      }
   }
   int test = 0;
   if ( stmt->expr ) {
      // Optimization:
      if ( stmt->expr->folded ) {
         t_add_opc( task, PCD_GOTO );
         t_add_arg( task, body );
      }
      else {
         test = t_tell( task );
         struct operand operand;
         init_operand( &operand );
         operand.push = true;
         operand.push_temp = true;
         do_operand( task, &operand, stmt->expr->root );
         t_add_opc( task, PCD_IFGOTO );
         t_add_arg( task, body );
      }
   }
   else {
      t_add_opc( task, PCD_GOTO );
      t_add_arg( task, body );
   }
   int done = t_tell( task );
   if ( stmt->expr ) {
      if ( ! stmt->expr->folded ) {
         t_seek( task, jump );
         t_add_opc( task, PCD_GOTO );
         t_add_arg( task, test );
      }
   }
   do_jump_target( task, stmt->jump_continue, next );
   do_jump_target( task, stmt->jump_break, done );
}

void do_jump( struct task* task, struct jump* jump ) {
   jump->obj_pos = t_tell( task );
   t_add_opc( task, PCD_GOTO );
   t_add_arg( task, 0 );
}

void do_jump_target( struct task* task, struct jump* jump, int pos ) {
   while ( jump ) {
      t_seek( task, jump->obj_pos );
      t_add_opc( task, PCD_GOTO );
      t_add_arg( task, pos );
      jump = jump->next;
   }
   t_seek( task, OBJ_SEEK_END );
}

void do_return( struct task* task, struct return_stmt* stmt ) {
   if ( stmt->packed_expr ) {
      struct operand operand;
      init_operand( &operand );
      operand.push = true;
      do_operand( task, &operand, stmt->packed_expr->expr->root );
      t_add_opc( task, PCD_RETURNVAL );
   }
   else {
      t_add_opc( task, PCD_RETURNVOID );
   }
}

void do_paltrans( struct task* task, struct paltrans* trans ) {
   push_expr( task, trans->number, true );
   t_add_opc( task, PCD_STARTTRANSLATION );
   struct palrange* range = trans->ranges;
   while ( range ) {
      push_expr( task, range->begin, true );
      push_expr( task, range->end, true );
      if ( range->rgb ) {
         push_expr( task, range->value.rgb.red1, true );
         push_expr( task, range->value.rgb.green1, true );
         push_expr( task, range->value.rgb.blue1, true );
         push_expr( task, range->value.rgb.red2, true );
         push_expr( task, range->value.rgb.green2, true );
         push_expr( task, range->value.rgb.blue2, true );
         t_add_opc( task, PCD_TRANSLATIONRANGE2 );
      }
      else {
         push_expr( task, range->value.ent.begin, true );
         push_expr( task, range->value.ent.end, true );
         t_add_opc( task, PCD_TRANSLATIONRANGE1 );
      }
      range = range->next;
   }
   t_add_opc( task, PCD_ENDTRANSLATION );
}

void do_default_params( struct task* task, struct func* func ) {
   struct param* param = func->params;
   while ( param && ! param->expr ) {
      param = param->next;
   }
   int num = 0;
   int num_cases = 0;
   struct param* start = param;
   while ( param ) {
      ++num;
      if ( ! param->expr->folded || param->expr->value ) {
         num_cases = num;
      }
      param = param->next;
   }
   if ( num_cases ) {
      // A hidden parameter is used to store the number of arguments passed to
      // the function. This parameter is found after the last visible parameter.
      t_add_opc( task, PCD_PUSHSCRIPTVAR );
      t_add_arg( task, func->max_param );
      int jump = t_tell( task );
      t_add_opc( task, PCD_CASEGOTOSORTED );
      t_add_arg( task, 0 );
      param = start;
      int i = 0;
      while ( param && i < num_cases ) {
         t_add_arg( task, 0 );
         t_add_arg( task, 0 );
         param = param->next;
         ++i;
      }
      t_add_opc( task, PCD_DROP );
      t_add_opc( task, PCD_GOTO );
      t_add_arg( task, 0 );
      param = start;
      while ( param ) {
         param->obj_pos = t_tell( task );
         if ( ! param->expr->folded || param->expr->value ) {
            push_expr( task, param->expr, false );
            update_indexed( task, STORAGE_LOCAL, param->index, AOP_NONE );
         }
         param = param->next;
      }
      int done = t_tell( task );
      // Add case positions.
      t_seek( task, jump );
      t_add_opc( task, PCD_CASEGOTOSORTED );
      t_add_arg( task, num_cases );
      num = func->min_param;
      param = start;
      while ( param && num_cases ) {
         t_add_arg( task, num );
         t_add_arg( task, param->obj_pos );
         param = param->next;
         --num_cases;
         ++num;
      }
      t_add_opc( task, PCD_DROP );
      t_add_opc( task, PCD_GOTO );
      t_add_arg( task, done );
      t_seek( task, done );
   }
   if ( start ) {
      // Reset the parameter-count parameter.
      t_add_opc( task, PCD_PUSHNUMBER );
      t_add_arg( task, 0 );
      t_add_opc( task, PCD_ASSIGNSCRIPTVAR );
      t_add_arg( task, func->max_param );
   }
}