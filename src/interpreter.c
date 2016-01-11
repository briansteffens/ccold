#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cold.h"

const bool DUMP_LOCALS = false;

int find_local(struct State* state, const char* name)
{
    for (int i = 0; i < state->local_count; i++)
        if (strcmp(state->locals[i]->name, name) == 0)
            return i;

    printf("Local %s not found\n", name);
    exit(0);
}

struct Local* get_local(struct State* state, const char* name)
{
    return state->locals[find_local(state, name)];
}

struct Value* value_clone(const struct Value* src)
{
    struct Value* ret = malloc(sizeof(struct Value));

    ret->type = src->type;
    ret->size = src->size;
    ret->data = malloc(ret->size);
    memcpy(ret->data, src->data, ret->size);

    return ret;
}

void value_free(const struct Value* value)
{
    free(value->data);
}

void local_free(struct Local* local)
{
    value_free(local->value);
    free(local);
}

void value_print(const struct Value* value)
{
    switch (value->type)
    {
    case TYPE_STRING:
        printf("STRING: \"%s\"\n", value->data);
        break;
    case TYPE_INT:
        printf("INT: \"%d\"\n", *((int*)value->data));
        break;
    default:
        printf("UNRECOGNIZED TYPE");
        break;
    }
}

void instruction_free(struct Instruction* inst)
{
    for (int i = 0; i < inst->param_count; i++)
        value_free(inst->params[i]->value);

    free(inst->params);
}

struct Value* resolve(struct State* state, struct Param* param)
{
    switch (param->type)
    {
    case PARAM_LABEL:
        return get_local(state, param->value->data)->value;
    case PARAM_LITERAL:
        return param->value;
    default:
        printf("Unrecognized param type: %d\n", param->type);
        exit(0);
    }
}

bool compare(struct Context* ctx, struct Value* left, struct Value* right)
{
    if (left->type == TYPE_INT && right->type == TYPE_INT)
    {
        return *((int*)left->data) == *((int*)right->data);
    }
    else if (left->type == TYPE_FLOAT && right->type == TYPE_FLOAT &&
             ctx->precision.type == TYPE_FLOAT)
    {
        float f_left = *((float*)left->data);
        float f_right = *((float*)right->data);
        float f_precision = *((float*)ctx->precision.data);
        return f_left >= f_right-f_precision && f_left <= f_right+f_precision;
    }
    else
    {
        printf("compare() can't support these types: %d == %d\n",
            left->type, right->type);
        exit(0);
    }
}

void dump_locals(struct State* state)
{
    for (int i = 0; i < state->local_count; i++)
        printf("LOCAL %s = %d\n", state->locals[i]->name,
               *((int*)state->locals[i]->value->data));
}

void print_program(struct Instruction** inst, int count, bool line_nums)
{
    const int BUF_LEN = 255;
    char buf[BUF_LEN];

    for (int i = 0; i < count; i++)
    {
        instruction_tostring(inst[i], buf, BUF_LEN);
        if (line_nums)
            printf("%d %s\n", i, buf);
        else
            printf("%s\n", i, buf);
    }
}

void interpret(struct State* state)
{
    const int BUF_LEN = 100;
    char buf[BUF_LEN];

    struct Instruction* inst = state->instructions[state->inst_ptr];

    //instruction_tostring(inst, buf, BUF_LEN);
    //printf("\t%d %s\n", state->inst_ptr, buf);
    /*
    printf("\n\n\n****\n");
    print_program(state->instructions, state->instruction_count, true);
    printf("****\n\n\n");
    printf("inst_ptr: %d\n", state->inst_ptr);
    */
    switch (inst->type)
    {
    case INST_LET:
        state->local_count++;
        state->locals = realloc(state->locals,
            state->local_count * sizeof(struct Local*));
        state->locals_owned = realloc(state->locals_owned,
            state->local_count * sizeof(bool));

        struct Local* local = malloc(sizeof(struct Local));

        int len = strlen(inst->params[0]->value->data);
        local->name = malloc(len);
        strncpy(local->name, inst->params[0]->value->data, len);

        local->value = value_clone(inst->params[1]->value);

        state->locals[state->local_count - 1] = local;
        state->locals_owned[state->local_count - 1] = true;

        break;
    case INST_ADD:;
    case INST_MUL:;
        int target = find_local(state, inst->params[0]->value->data);

        struct Value* left = resolve(state, inst->params[1]);
        struct Value* right = resolve(state, inst->params[2]);

        struct Local* new_local = malloc(sizeof(struct Local));
        new_local->name = state->locals[target]->name;
        new_local->value = malloc(sizeof(struct Value));

        if (left->type == TYPE_INT && right->type == TYPE_INT &&
            state->locals[target]->value->type == TYPE_INT)
        {
            switch (inst->type)
            {
            case INST_ADD:
                value_set_int(new_local->value,
                    *((int*)left->data)+*((int*)right->data));
                break;
            case INST_MUL:
                value_set_int(new_local->value,
                    *((int*)left->data)**((int*)right->data));
                break;
            }
        }
        else if (left->type == TYPE_FLOAT && right->type == TYPE_FLOAT &&
                 state->locals[target]->value->type == TYPE_FLOAT)
        {
            switch (inst->type)
            {
            case INST_ADD:
                value_set_float(new_local->value,
                    *((float*)left->data)+*((float*)right->data));
                break;
            case INST_MUL:
                value_set_float(new_local->value,
                    *((float*)left->data)**((float*)right->data));
                break;
            }
        }
        else
        {
            printf("interpret() can't handle these types: %d, %d, %d\n",
                left->type, right->type, state->locals[target]->value->type);
            exit(0);
        }

        state->locals[target] = new_local;
        state->locals_owned[target] = true;

        break;
    case INST_JUMP:
        state->inst_ptr = *((int*)inst->params[0]->value->data);
        return;
    case INST_CMP:;
        struct Value* cmp_left = resolve(state, inst->params[0]);
        struct Value* cmp_right = resolve(state, inst->params[1]);

        if (cmp_left->type != TYPE_INT || cmp_right->type != TYPE_INT)
        {
            printf("Type system can't support this\n");
            exit(0);
        }

        if (*((int*)cmp_left->data) == *((int*)cmp_right->data))
        {
            state->inst_ptr = *((int*)inst->params[2]->value->data);
            return;
        }

        break;
    case INST_PRINT:;
        struct Value* print_target = resolve(state, inst->params[0]);

        switch (print_target->type)
        {
        case TYPE_INT:
            printf("%d\n", *((int*)print_target->data));
            break;
        default:
            printf("Can't print this type\n");
            exit(0);
        }

        break;
    case INST_RET:
        state->ret = resolve(state, inst->params[0]);
        state->inst_ptr = state->instruction_count;
        break;
    default:
        printf("Unrecognized instruction type: %d\n", inst->type);
        exit(0);
    }

    if (DUMP_LOCALS)
    {
        dump_locals(state);
        printf("\n");
    }

    state->inst_ptr++;
}

void free_state(struct State* state)
{
    for (int i = 0; i < state->instruction_count; i++)
        if (state->instructions_owned[i])
            instruction_free(state->instructions[i]);

    free(state->instructions);

    for (int i = 0; i < state->local_count; i++)
        if (state->locals_owned[i])
            local_free(state->locals[i]);

    free(state->locals);

    free(state);
}

struct State* setup_state(struct Context* ctx, int case_index)
{
    struct State* ret = malloc(1 * sizeof(struct State));

    ret->local_count = ctx->input_count;
    ret->locals = malloc(ret->local_count * sizeof(struct Local*));
    ret->locals_owned = malloc(ret->local_count * sizeof(bool));

    for (int i = 0; i < ctx->input_count; i++)
    {
        ret->locals[i] = malloc(sizeof(struct Local));
        ret->locals[i]->name = ctx->input_names[i];
        ret->locals[i]->value = value_clone(&ctx->cases[case_index].input_values[i]);

        ret->locals_owned[i] = true;
    }

    ret->ret = NULL;

    return ret;
}
