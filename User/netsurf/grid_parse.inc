/* Keep the grid track list in the computed style, including cascade and media rules. */
#include "bytecode/opcodes.h"
#include "parse/properties/utils.h"
#include <string.h>

static css_error grid_tokens(css_language *c, const parserutils_vector *v,
        int32_t *ctx, css_style *result, uint32_t prop, bool single)
{
    char value[1024];
    size_t used = 0;
    int32_t cursor = *ctx;
    const css_token *t;
    while ((t = parserutils_vector_peek(v, cursor)) != NULL) {
        if (t->type == CSS_TOKEN_CHAR && lwc_string_data(t->idata)[0] == '!') break;
        if (single && used && t->type == CSS_TOKEN_S) break;
        const char *s = t->type == CSS_TOKEN_S ? " " :
                t->idata ? lwc_string_data(t->idata) : NULL;
        size_t len = t->type == CSS_TOKEN_S ? 1 : t->idata ? lwc_string_length(t->idata) : 0;
        if (!s || len >= sizeof(value)-used-1) return CSS_INVALID;
        memcpy(value+used, s, len); used += len;
        if (t->type == CSS_TOKEN_FUNCTION) value[used++] = '(';
        if (t->type == CSS_TOKEN_PERCENTAGE) value[used++] = '%';
        cursor++;
        if (single) break;
    }
    if (!used) return CSS_INVALID;
    value[used] = 0;
    lwc_string *intern;
    if (lwc_intern_string(value, used, &intern) != lwc_error_ok) return CSS_NOMEM;
    uint32_t index;
    css_error err = css__stylesheet_string_add(c->sheet, intern, &index);
    if (err == CSS_OK) err = css__stylesheet_style_appendOPV(result,prop,0,LIST_STYLE_IMAGE_URI);
    if (err == CSS_OK) err = css__stylesheet_style_append(result,index);
    if (err == CSS_OK) *ctx = cursor;
    return err;
}

#define GRID_PARSER(name, prop, single) \
static css_error parse_##name(css_language *c, const parserutils_vector *v, int32_t *ctx, css_style *r) { \
    return grid_tokens(c,v,ctx,r,CSS_PROP_##prop,single); \
}
GRID_PARSER(grid_template_columns, GRID_TEMPLATE_COLUMNS, false)
GRID_PARSER(grid_template_rows, GRID_TEMPLATE_ROWS, false)
GRID_PARSER(grid_column, GRID_COLUMN, false)
GRID_PARSER(row_gap, ROW_GAP, true)
GRID_PARSER(inset_inline_start, INSET_INLINE_START, true)
GRID_PARSER(inset_inline_end, INSET_INLINE_END, true)
GRID_PARSER(transform, TRANSFORM, false)

static css_error parse_gap(css_language *c, const parserutils_vector *v, int32_t *ctx, css_style *r)
{
    int32_t first = *ctx, second = first;
    css_error err = parse_row_gap(c,v,&second,r);
    if (err != CSS_OK) return err;
    consumeWhitespace(v,&second);
    const css_token *next = parserutils_vector_peek(v,second);
    *ctx = (!next || tokenIsChar(next,'!')) ? first : second;
    return css__parse_column_gap(c,v,ctx,r);
}

/* A single nonnegative length or percentage sets all four corners. */
static css_error parse_border_radius(css_language *c, const parserutils_vector *v,
        int32_t *ctx, css_style *r)
{
    return css__parse_padding_side(c, v, ctx, r, CSS_PROP_BORDER_RADIUS);
}

/* The framebuffer lays text out in horizontal writing mode. A one-value
 * logical padding is symmetric regardless of the inline direction. */
static css_error symmetric_padding(css_language *c, const parserutils_vector *v,
        int32_t *ctx, css_style *r, uint32_t start, uint32_t end)
{
    int32_t cursor = *ctx;
    css_error err = css__parse_padding_side(c, v, &cursor, r, start);
    if (err != CSS_OK) return err;
    consumeWhitespace(v, &cursor);
    const css_token *next = parserutils_vector_peek(v, cursor);
    if (next && !tokenIsChar(next,'!')) return CSS_INVALID;
    return css__parse_padding_side(c, v, ctx, r, end);
}
static css_error parse_padding_block(css_language *c, const parserutils_vector *v,
        int32_t *ctx, css_style *r)
{
    return symmetric_padding(c,v,ctx,r,CSS_PROP_PADDING_TOP,CSS_PROP_PADDING_BOTTOM);
}
static css_error parse_padding_inline(css_language *c, const parserutils_vector *v,
        int32_t *ctx, css_style *r)
{
    return symmetric_padding(c,v,ctx,r,CSS_PROP_PADDING_LEFT,CSS_PROP_PADDING_RIGHT);
}
