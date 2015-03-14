#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "phase.h"

static void read_block( struct parse* phase, struct stmt_reading* );
static void read_case( struct parse* phase, struct stmt_reading* );
static void read_default_case( struct parse* phase, struct stmt_reading* );
static void read_label( struct parse* phase, struct stmt_reading* );
static void read_stmt( struct parse* phase, struct stmt_reading* );
static void read_if( struct parse* phase, struct stmt_reading* );
static void read_switch( struct parse* phase, struct stmt_reading* );
static void read_while( struct parse* phase, struct stmt_reading* );
static void read_do( struct parse* phase, struct stmt_reading* );
static void read_for( struct parse* phase, struct stmt_reading* );
static void read_jump( struct parse* phase, struct stmt_reading* );
static void read_script_jump( struct parse* phase, struct stmt_reading* );
static void read_return( struct parse* phase, struct stmt_reading* );
static void read_goto( struct parse* phase, struct stmt_reading* );
static void read_paltrans( struct parse* phase, struct stmt_reading* );
static void read_format_item( struct parse* phase, struct stmt_reading* );
static void read_palrange_rgb_field( struct parse* phase, struct expr**,
   struct expr**, struct expr** );
static void read_packed_expr( struct parse* phase, struct stmt_reading* );
static struct label* alloc_label( char*, struct pos );
static struct path* alloc_path( struct pos );

void t_print_name( struct name* name ) {
   struct str str;
   str_init( &str );
   t_copy_name( name, true, &str );
   printf( "%s\n", str.value );
   str_deinit( &str );
}

// Gets the top-most object associated with the name, and only retrieves the
// object if it can be used by the current module.
struct object* t_get_region_object( struct task* task, struct region* region,
   struct name* name ) {
   struct object* object = name->object;
   if ( ! object ) {
      return NULL;
   }
   // Find the top-most object.
   while ( object && object->next_scope ) {
      object = object->next_scope;
   }
   if ( object->depth != 0 ) {
      return NULL;
   }
   return object;
}

void p_init_stmt_reading( struct stmt_reading* reading, struct list* labels ) {
   reading->labels = labels;
   reading->node = NULL;
   reading->block_node = NULL;
}

void p_read_top_stmt( struct parse* phase, struct stmt_reading* reading,
   bool need_block ) {
   if ( need_block ) {
      read_block( phase, reading );
   }
   else {
      read_stmt( phase, reading );
   }
   // All goto statements need to refer to valid labels.
   if ( reading->node->type == NODE_BLOCK ) {
      list_iter_t i;
      list_iter_init( &i, reading->labels );
      while ( ! list_end( &i ) ) {
         struct label* label = list_data( &i );
         if ( ! label->defined ) {
            p_diag( phase, DIAG_POS_ERR, &label->pos,
               "label `%s` not found", label->name );
            p_bail( phase );
         }
         list_next( &i );
      }
   }
}

void read_block( struct parse* phase, struct stmt_reading* reading ) {
   p_test_tk( phase, TK_BRACE_L );
   struct block* block = mem_alloc( sizeof( *block ) );
   block->node.type = NODE_BLOCK;
   list_init( &block->stmts );
   block->pos = phase->tk_pos;
   p_read_tk( phase );
   while ( true ) {
      if ( p_is_dec( phase ) ) {
         struct dec dec;
         p_init_dec( &dec );
         dec.area = DEC_LOCAL;
         dec.name_offset = phase->region->body;
         dec.vars = &block->stmts;
         p_read_dec( phase, &dec );
      }
      else if ( phase->tk == TK_CASE ) {
         read_case( phase, reading );
         list_append( &block->stmts, reading->node );
      }
      else if ( phase->tk == TK_DEFAULT ) {
         read_default_case( phase, reading );
         list_append( &block->stmts, reading->node );
      }
      else if ( phase->tk == TK_ID && p_peek( phase ) == TK_COLON ) {
         read_label( phase, reading );
         list_append( &block->stmts, reading->node );
      }
      else if ( phase->tk == TK_IMPORT ) {
         p_read_import( phase, &block->stmts );
      }
      else if ( phase->tk == TK_BRACE_R ) {
         p_read_tk( phase );
         break;
      }
      else {
         read_stmt( phase, reading );
         if ( reading->node->type != NODE_NONE ) {
            list_append( &block->stmts, reading->node );
         }
      }
   }
   reading->node = &block->node;
   reading->block_node = block;
}

void read_case( struct parse* phase, struct stmt_reading* reading ) {
   struct case_label* label = mem_alloc( sizeof( *label ) );
   label->node.type = NODE_CASE;
   label->offset = 0;
   label->next = NULL;
   label->pos = phase->tk_pos;
   p_read_tk( phase );
   struct expr_reading number;
   p_init_expr_reading( &number, false, false, false, true );
   p_read_expr( phase, &number );
   label->number = number.output_node;
   p_test_tk( phase, TK_COLON );
   p_read_tk( phase );
   reading->node = &label->node;
}

void read_default_case( struct parse* phase, struct stmt_reading* reading ) {
   struct case_label* label = mem_alloc( sizeof( *label ) );
   label->node.type = NODE_CASE_DEFAULT;
   label->pos = phase->tk_pos;
   label->offset = 0;
   p_read_tk( phase );
   p_test_tk( phase, TK_COLON );
   p_read_tk( phase );
   reading->node = &label->node;
}

void read_label( struct parse* phase, struct stmt_reading* reading ) {
   struct label* label = NULL;
   list_iter_t i;
   list_iter_init( &i, reading->labels );
   while ( ! list_end( &i ) ) {
      struct label* prev = list_data( &i );
      if ( strcmp( prev->name, phase->tk_text ) == 0 ) {
         label = prev;
         break;
      }
      list_next( &i );
   }
   if ( label ) {
      if ( label->defined ) {
         p_diag( phase, DIAG_POS_ERR, &phase->tk_pos,
            "duplicate label `%s`", phase->tk_text );
         p_diag( phase, DIAG_FILE | DIAG_LINE | DIAG_COLUMN, &label->pos,
            "label already found here" );
         p_bail( phase );
      }
      else {
         label->defined = true;
         label->pos = phase->tk_pos;
      }
   }
   else {
      label = alloc_label( phase->tk_text, phase->tk_pos );
      label->defined = true;
      list_append( reading->labels, label );
   }
   p_read_tk( phase );
   p_read_tk( phase );
   reading->node = &label->node;
}

struct label* alloc_label( char* name, struct pos pos ) {
   struct label* label = mem_alloc( sizeof( *label ) );
   label->node.type = NODE_GOTO_LABEL;
   label->name = name;
   label->defined = false;
   label->pos = pos;
   label->users = NULL;
   label->obj_pos = 0;
   label->format_block = NULL;
   return label;
}

void read_stmt( struct parse* phase, struct stmt_reading* reading ) {
   switch ( phase->tk ) {
   case TK_BRACE_L:
      read_block( phase, reading );
      break;
   case TK_IF:
      read_if( phase, reading );
      break;
   case TK_SWITCH:
      read_switch( phase, reading );
      break;
   case TK_WHILE:
   case TK_UNTIL:
      read_while( phase, reading );
      break;
   case TK_DO:
      read_do( phase, reading );
      break;
   case TK_FOR:
      read_for( phase, reading );
      break;
   case TK_BREAK:
   case TK_CONTINUE:
      read_jump( phase, reading );
      break;
   case TK_TERMINATE:
   case TK_RESTART:
   case TK_SUSPEND:
      read_script_jump( phase, reading );
      break;
   case TK_RETURN:
      read_return( phase, reading );
      break;
   case TK_GOTO:
      read_goto( phase, reading );
      break;
   case TK_PALTRANS:
      read_paltrans( phase, reading );
      break;
   case TK_SEMICOLON:
      {
         static struct node node = { NODE_NONE };
         reading->node = &node;
         p_read_tk( phase );
      }
      break;
   default:
      // Format item in a format block:
      if ( phase->tk == TK_ID && p_peek( phase ) == TK_ASSIGN_COLON ) {
         read_format_item( phase, reading );
      }
      else {
         read_packed_expr( phase, reading );
      }
   }
}

void read_if( struct parse* phase, struct stmt_reading* reading ) {
   p_read_tk( phase );
   struct if_stmt* stmt = mem_alloc( sizeof( *stmt ) );
   stmt->node.type = NODE_IF;
   p_test_tk( phase, TK_PAREN_L );
   p_read_tk( phase );
   struct expr_reading cond;
   p_init_expr_reading( &cond, false, false, false, true );
   p_read_expr( phase, &cond );
   stmt->cond = cond.output_node;
   p_test_tk( phase, TK_PAREN_R );
   p_read_tk( phase );
   // Warn when the body of an `if` statement is empty. It is assumed that a
   // semicolon is the empty statement.
   if ( phase->tk == TK_SEMICOLON ) {
      p_diag( phase, DIAG_WARN | DIAG_FILE | DIAG_LINE | DIAG_COLUMN,
         &phase->tk_pos, "body of `if` statement is empty (`;`)" );
   }
   read_stmt( phase, reading );
   stmt->body = reading->node;
   stmt->else_body = NULL;
   if ( phase->tk == TK_ELSE ) {
      p_read_tk( phase );
      if ( phase->tk == TK_SEMICOLON ) {
         p_diag( phase, DIAG_WARN | DIAG_FILE | DIAG_LINE | DIAG_COLUMN,
            &phase->tk_pos, "body of `else` is empty (`;`)" );
      }
      read_stmt( phase, reading );
      stmt->else_body = reading->node;
   }
   reading->node = &stmt->node;
}

void read_switch( struct parse* phase, struct stmt_reading* reading ) {
   p_read_tk( phase );
   struct switch_stmt* stmt = mem_alloc( sizeof( *stmt ) );
   stmt->node.type = NODE_SWITCH;
   p_test_tk( phase, TK_PAREN_L );
   p_read_tk( phase );
   struct expr_reading cond;
   p_init_expr_reading( &cond, false, false, false, true );
   p_read_expr( phase, &cond );
   stmt->cond = cond.output_node;
   p_test_tk( phase, TK_PAREN_R );
   p_read_tk( phase );
   read_stmt( phase, reading );
   stmt->body = reading->node;
   reading->node = &stmt->node;
}

void read_while( struct parse* phase, struct stmt_reading* reading ) {
   struct while_stmt* stmt = mem_alloc( sizeof( *stmt ) );
   stmt->node.type = NODE_WHILE;
   stmt->type = WHILE_WHILE;
   if ( phase->tk == TK_WHILE ) {
      p_read_tk( phase );
   }
   else {
      p_test_tk( phase, TK_UNTIL );
      p_read_tk( phase );
      stmt->type = WHILE_UNTIL;
   }
   p_test_tk( phase, TK_PAREN_L );
   p_read_tk( phase );
   struct expr_reading cond;
   p_init_expr_reading( &cond, false, false, false, true );
   p_read_expr( phase, &cond );
   stmt->cond = cond.output_node;
   p_test_tk( phase, TK_PAREN_R );
   p_read_tk( phase );
   read_stmt( phase, reading );
   stmt->body = reading->node;
   stmt->jump_break = NULL;
   stmt->jump_continue = NULL;
   reading->node = &stmt->node;
}

void read_do( struct parse* phase, struct stmt_reading* reading ) {
   p_read_tk( phase );
   struct while_stmt* stmt = mem_alloc( sizeof( *stmt ) );
   stmt->node.type = NODE_WHILE;
   stmt->type = WHILE_DO_WHILE;
   read_stmt( phase, reading );
   stmt->body = reading->node;
   stmt->jump_break = NULL;
   stmt->jump_continue = NULL;
   if ( phase->tk == TK_WHILE ) {
      p_read_tk( phase );
   }
   else {
      p_test_tk( phase, TK_UNTIL );
      p_read_tk( phase );
      stmt->type = WHILE_DO_UNTIL;
   }
   p_test_tk( phase, TK_PAREN_L );
   p_read_tk( phase );
   struct expr_reading cond;
   p_init_expr_reading( &cond, false, false, false, true );
   p_read_expr( phase, &cond );
   stmt->cond = cond.output_node;
   p_test_tk( phase, TK_PAREN_R );
   p_read_tk( phase );
   p_test_tk( phase, TK_SEMICOLON );
   p_read_tk( phase );
   reading->node = &stmt->node;
}

void read_for( struct parse* phase, struct stmt_reading* reading ) {
   p_read_tk( phase );
   struct for_stmt* stmt = mem_alloc( sizeof( *stmt ) );
   stmt->node.type = NODE_FOR;
   list_init( &stmt->init );
   list_init( &stmt->post );
   stmt->cond = NULL;
   stmt->body = NULL;
   stmt->jump_break = NULL;
   stmt->jump_continue = NULL;
   p_test_tk( phase, TK_PAREN_L );
   p_read_tk( phase );
   // Optional initialization:
   if ( phase->tk != TK_SEMICOLON ) {
      if ( p_is_dec( phase ) ) {
         struct dec dec;
         p_init_dec( &dec );
         dec.area = DEC_FOR;
         dec.name_offset = phase->region->body;
         dec.vars = &stmt->init;
         p_read_dec( phase, &dec );
      }
      else {
         while ( true ) {
            struct expr_reading expr;
            p_init_expr_reading( &expr, false, false, false, true );
            p_read_expr( phase, &expr );
            list_append( &stmt->init, expr.output_node );
            if ( phase->tk == TK_COMMA ) {
               p_read_tk( phase );
            }
            else {
               break;
            }
         }
         p_test_tk( phase, TK_SEMICOLON );
         p_read_tk( phase );
      }
   }
   else {
      p_read_tk( phase );
   }
   // Optional condition:
   if ( phase->tk != TK_SEMICOLON ) {
      struct expr_reading cond;
      p_init_expr_reading( &cond, false, false, false, true );
      p_read_expr( phase, &cond );
      stmt->cond = cond.output_node;
      p_test_tk( phase, TK_SEMICOLON );
      p_read_tk( phase );
   }
   else {
      p_read_tk( phase );
   }
   // Optional post-expression:
   if ( phase->tk != TK_PAREN_R ) {
      while ( true ) {
         struct expr_reading expr;
         p_init_expr_reading( &expr, false, false, false, true );
         p_read_expr( phase, &expr );
         list_append( &stmt->post, expr.output_node );
         if ( phase->tk == TK_COMMA ) {
            p_read_tk( phase );
         }
         else {
            break;
         }
      }
   }
   p_test_tk( phase, TK_PAREN_R );
   p_read_tk( phase );
   read_stmt( phase, reading );
   stmt->body = reading->node;
   reading->node = &stmt->node;
}

void read_jump( struct parse* phase, struct stmt_reading* reading ) {
   struct jump* stmt = mem_alloc( sizeof( *stmt ) );
   stmt->node.type = NODE_JUMP;
   stmt->type = JUMP_BREAK;
   stmt->next = NULL;
   stmt->pos = phase->tk_pos;
   stmt->obj_pos = 0;
   if ( phase->tk == TK_CONTINUE ) {
      stmt->type = JUMP_CONTINUE;
   }
   p_read_tk( phase );
   p_test_tk( phase, TK_SEMICOLON );
   p_read_tk( phase );
   reading->node = &stmt->node;
}

void read_script_jump( struct parse* phase, struct stmt_reading* reading ) {
   struct script_jump* stmt = mem_alloc( sizeof( *stmt ) );
   stmt->node.type = NODE_SCRIPT_JUMP;
   stmt->type = SCRIPT_JUMP_TERMINATE;
   stmt->pos = phase->tk_pos;
   if ( phase->tk == TK_RESTART ) {
      stmt->type = SCRIPT_JUMP_RESTART;
   }
   else if ( phase->tk == TK_SUSPEND ) {
      stmt->type = SCRIPT_JUMP_SUSPEND;
   }
   p_read_tk( phase );
   p_test_tk( phase, TK_SEMICOLON );
   p_read_tk( phase );
   reading->node = &stmt->node;
}

void read_return( struct parse* phase, struct stmt_reading* reading ) {
   struct return_stmt* stmt = mem_alloc( sizeof( *stmt ) );
   stmt->node.type = NODE_RETURN;
   stmt->return_value = NULL;
   stmt->pos = phase->tk_pos;
   p_read_tk( phase );
   if ( phase->tk == TK_SEMICOLON ) {
      p_read_tk( phase );
   }
   else {
      read_packed_expr( phase, reading );
      stmt->return_value = ( struct packed_expr* ) reading->node;
   }
   reading->node = &stmt->node;
}

void read_goto( struct parse* phase, struct stmt_reading* reading ) {
   struct pos pos = phase->tk_pos;
   p_read_tk( phase );
   p_test_tk( phase, TK_ID );
   struct label* label = NULL;
   list_iter_t i;
   list_iter_init( &i, reading->labels );
   while ( ! list_end( &i ) ) {
      struct label* prev = list_data( &i );
      if ( strcmp( prev->name, phase->tk_text ) == 0 ) {
         label = prev;
         break;
      }
      list_next( &i );
   }
   if ( ! label ) {
      label = alloc_label( phase->tk_text, phase->tk_pos );
      list_append( reading->labels, label );
   }
   p_read_tk( phase );
   p_test_tk( phase, TK_SEMICOLON );
   p_read_tk( phase );
   struct goto_stmt* stmt = mem_alloc( sizeof( *stmt ) );
   stmt->node.type = NODE_GOTO;
   stmt->label = label;
   stmt->next = label->users;
   label->users = stmt;
   stmt->obj_pos = 0;
   stmt->format_block = NULL;
   stmt->pos = pos;
   reading->node = &stmt->node;
}

void read_paltrans( struct parse* phase, struct stmt_reading* reading ) {
   p_read_tk( phase );
   struct paltrans* stmt = mem_alloc( sizeof( *stmt ) );
   stmt->node.type = NODE_PALTRANS;
   stmt->ranges = NULL;
   stmt->ranges_tail = NULL;
   p_test_tk( phase, TK_PAREN_L );
   p_read_tk( phase );
   struct expr_reading expr;
   p_init_expr_reading( &expr, false, false, false, true );
   p_read_expr( phase, &expr );
   stmt->number = expr.output_node;
   while ( phase->tk == TK_COMMA ) {
      struct palrange* range = mem_alloc( sizeof( *range ) );
      range->next = NULL;
      p_read_tk( phase );
      p_init_expr_reading( &expr, false, false, false, true );
      p_read_expr( phase, &expr );
      range->begin = expr.output_node;
      p_test_tk( phase, TK_COLON );
      p_read_tk( phase );
      p_init_expr_reading( &expr, false, true, false, true );
      p_read_expr( phase, &expr );
      range->end = expr.output_node;
      p_test_tk( phase, TK_ASSIGN );
      p_read_tk( phase );
      if ( phase->tk == TK_BRACKET_L ) {
         read_palrange_rgb_field( phase, &range->value.rgb.red1,
            &range->value.rgb.green1, &range->value.rgb.blue1 );
         p_test_tk( phase, TK_COLON );
         p_read_tk( phase );
         read_palrange_rgb_field( phase, &range->value.rgb.red2,
            &range->value.rgb.green2, &range->value.rgb.blue2 );
         range->rgb = true;
      }
      else {
         p_init_expr_reading( &expr, false, false, false, true );
         p_read_expr( phase, &expr );
         range->value.ent.begin = expr.output_node;
         p_test_tk( phase, TK_COLON );
         p_read_tk( phase );
         p_init_expr_reading( &expr, false, false, false, true );
         p_read_expr( phase, &expr );
         range->value.ent.end = expr.output_node;
         range->rgb = false;
      }
      if ( stmt->ranges ) {
         stmt->ranges_tail->next = range;
         stmt->ranges_tail = range;
      }
      else {
         stmt->ranges = range;
         stmt->ranges_tail = range;
      }
   }
   p_test_tk( phase, TK_PAREN_R );
   p_read_tk( phase );
   p_test_tk( phase, TK_SEMICOLON );
   p_read_tk( phase );
   reading->node = &stmt->node;
}

void read_palrange_rgb_field( struct parse* phase, struct expr** r,
   struct expr** g, struct expr** b ) {
   p_test_tk( phase, TK_BRACKET_L );
   p_read_tk( phase );
   struct expr_reading expr;
   p_init_expr_reading( &expr, false, false, false, true );
   p_read_expr( phase, &expr );
   *r = expr.output_node;
   p_test_tk( phase, TK_COMMA );
   p_read_tk( phase );
   p_init_expr_reading( &expr, false, false, false, true );
   p_read_expr( phase, &expr );
   *g = expr.output_node;
   p_test_tk( phase, TK_COMMA );
   p_read_tk( phase );
   p_init_expr_reading( &expr, false, false, false, true );
   p_read_expr( phase, &expr );
   *b = expr.output_node;
   p_test_tk( phase, TK_BRACKET_R );
   p_read_tk( phase ); 
}

void read_format_item( struct parse* phase, struct stmt_reading* reading ) {
   struct format_item* item = p_read_format_item( phase, false );
   reading->node = &item->node;
   p_test_tk( phase, TK_SEMICOLON );
   p_read_tk( phase );
}

void read_packed_expr( struct parse* phase, struct stmt_reading* reading ) {
   struct expr_reading expr;
   p_init_expr_reading( &expr, false, false, false, false );
   p_read_expr( phase, &expr );
   struct packed_expr* packed = mem_alloc( sizeof( *packed ) );
   packed->node.type = NODE_PACKED_EXPR;
   packed->expr = expr.output_node;
   packed->block = NULL;
   // With format block.
   if ( phase->tk == TK_ASSIGN_COLON ) {
      p_read_tk( phase );
      read_block( phase, reading );
      packed->block = reading->block_node;
   }
   else {
      p_test_tk( phase, TK_SEMICOLON );
      p_read_tk( phase );
   }
   reading->node = &packed->node;
}

void p_read_import( struct parse* phase, struct list* local ) {
   p_test_tk( phase, TK_IMPORT );
   struct import* stmt = mem_alloc( sizeof( *stmt ) );
   stmt->node.type = NODE_IMPORT;
   stmt->pos = phase->tk_pos;
   stmt->path = NULL;
   stmt->item = NULL;
   stmt->next = NULL;
   p_read_tk( phase );
   stmt->path = p_read_path( phase );
   p_test_tk( phase, TK_COLON );
   p_read_tk( phase );
   struct import_item* tail = NULL;
   while ( true ) {
      struct import_item* item = mem_alloc( sizeof( *item ) );
      item->pos = phase->tk_pos;
      item->next = NULL;
      item->name = NULL;
      item->alias = NULL;
      item->is_struct = false;
      item->is_link = false;
      // Link with another region.
      if ( phase->tk == TK_REGION ) {
         p_read_tk( phase );
         p_test_tk( phase, TK_ASSIGN );
         p_read_tk( phase );
         item->is_link = true;
         // Link with child region of selected region.
         if ( phase->tk == TK_ID ) {
            item->name = phase->tk_text;
            item->name_pos = phase->tk_pos;
            p_read_tk( phase );
         }
         // Link with selected region.
         else {
            p_test_tk( phase, TK_REGION );
            p_read_tk( phase );
         }
      }
      else {
         // Import structure.
         if ( phase->tk == TK_STRUCT ) {
            item->is_struct = true;
            p_read_tk( phase );
            p_test_tk( phase, TK_ID );
            item->name = phase->tk_text;
            item->name_pos = phase->tk_pos;
            p_read_tk( phase );
         }
         // Import object.
         else {
            p_test_tk( phase, TK_ID );
            item->name = phase->tk_text;
            item->name_pos = phase->tk_pos;
            p_read_tk( phase );
         }
         // Alias for imported object.
         if ( phase->tk == TK_ASSIGN ) {
            item->alias = item->name;
            item->alias_pos = item->name_pos;
            p_read_tk( phase );
            // Alias to the selected region. Only do this if "struct" was not
            // specified.
            if ( phase->tk == TK_REGION && ! item->is_struct ) {
               item->name = NULL;
               p_read_tk( phase );
            }
            else {
               p_test_tk( phase, TK_ID );
               item->name = phase->tk_text;
               item->name_pos = phase->tk_pos;
               p_read_tk( phase );
            }
         }
      }
      if ( tail ) {
         tail->next = item;
      }
      else {
         stmt->item = item;
      }
      tail = item;
      if ( phase->tk == TK_COMMA ) {
         p_read_tk( phase );
      }
      else {
         break;
      }
   }
   p_test_tk( phase, TK_SEMICOLON );
   p_read_tk( phase );
   if ( local ) {
      list_append( local, stmt );
   }
   else {
      list_append( &phase->region->imports, stmt );
   }
}

struct path* p_read_path( struct parse* phase ) {
   // Head of path.
   struct path* path = alloc_path( phase->tk_pos );
   if ( phase->tk == TK_UPMOST ) {
      path->is_upmost = true;
      p_read_tk( phase );
   }
   else if ( phase->tk == TK_REGION ) {
      path->is_region = true;
      p_read_tk( phase );
   }
   else {
      p_test_tk( phase, TK_ID );
      path->text = phase->tk_text;
      p_read_tk( phase );
   }
   // Tail of path.
   struct path* head = path;
   struct path* tail = head;
   while ( phase->tk == TK_COLON_2 ) {
      p_read_tk( phase );
      p_test_tk( phase, TK_ID );
      path = alloc_path( phase->tk_pos );
      path->text = phase->tk_text;
      tail->next = path;
      tail = path;
      p_read_tk( phase );
   }
   return head;
}

struct path* alloc_path( struct pos pos ) {
   struct path* path = mem_alloc( sizeof( *path ) );
   path->next = NULL;
   path->text = NULL;
   path->pos = pos;
   path->is_region = false;
   path->is_upmost = false;
   return path;
}