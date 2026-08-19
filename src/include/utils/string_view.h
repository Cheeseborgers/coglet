#ifndef COGLET_STRING_VIEW_H
#define COGLET_STRING_VIEW_H

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

/*
    StringView

    A non-owning reference to a string.

    It does NOT:
        - allocate memory
        - copy data
        - free memory

    The referenced memory must stay alive while the view is used.

    Example:

        const char *text = "hello world";

        StringView view = string_view(text, 5);

        represents:
            "hello"
*/

typedef struct {
    const char *data;
    size_t length;
} StringView;


/* ---------------------------------------------------------
    Constructors
--------------------------------------------------------- */

static inline StringView string_view(const char *data, size_t length)
{
    return (StringView){
        .data = data,
        .length = length
    };
}


static inline StringView string_view_from_cstr(const char *str)
{
    size_t length = 0;

    while (str[length])
        length++;

    return string_view(str, length);
}


static inline StringView string_view_empty(void)
{
    return string_view(NULL, 0);
}


/* ---------------------------------------------------------
    Properties
--------------------------------------------------------- */

static inline bool string_view_is_empty(StringView view)
{
    return view.length == 0;
}


static inline char string_view_at(StringView view, size_t index)
{
    return view.data[index];
}


/* ---------------------------------------------------------
    Slicing
--------------------------------------------------------- */

static inline StringView string_view_substr(
    StringView view,
    size_t start,
    size_t length
)
{
    if (start >= view.length)
        return string_view_empty();

    if (start + length > view.length)
        length = view.length - start;

    return string_view(
        view.data + start,
        length
    );
}


static inline StringView string_view_slice(
    StringView view,
    size_t start,
    size_t end
)
{
    if (start >= view.length || start >= end)
        return string_view_empty();

    if (end > view.length)
        end = view.length;

    return string_view(
        view.data + start,
        end - start
    );
}


/* ---------------------------------------------------------
    Comparison
--------------------------------------------------------- */

static inline bool string_view_equals(
    StringView a,
    StringView b
)
{
    if (a.length != b.length)
        return false;

    for (size_t i = 0; i < a.length; i++)
    {
        if (a.data[i] != b.data[i])
            return false;
    }

    return true;
}


static inline bool string_view_equals_cstr(
    StringView view,
    const char *str
)
{
    return string_view_equals(
        view,
        string_view_from_cstr(str)
    );
}


/* ---------------------------------------------------------
    Searching
--------------------------------------------------------- */

static inline bool string_view_contains(
    StringView view,
    char c
)
{
    for (size_t i = 0; i < view.length; i++)
    {
        if (view.data[i] == c)
            return true;
    }

    return false;
}


static inline size_t string_view_find(
    StringView view,
    char c
)
{
    for (size_t i = 0; i < view.length; i++)
    {
        if (view.data[i] == c)
            return i;
    }

    return (size_t)-1;
}


/* ---------------------------------------------------------
    Prefix / suffix
--------------------------------------------------------- */

static inline bool string_view_starts_with(
    StringView view,
    StringView prefix
)
{
    if (prefix.length > view.length)
        return false;

    for (size_t i = 0; i < prefix.length; i++)
    {
        if (view.data[i] != prefix.data[i])
            return false;
    }

    return true;
}


static inline bool string_view_ends_with(
    StringView view,
    StringView suffix
)
{
    if (suffix.length > view.length)
        return false;

    size_t offset = view.length - suffix.length;

    for (size_t i = 0; i < suffix.length; i++)
    {
        if (view.data[offset + i] != suffix.data[i])
            return false;
    }

    return true;
}


/* ---------------------------------------------------------
    Trimming
--------------------------------------------------------- */

static inline StringView string_view_trim_left(
    StringView view
)
{
    size_t start = 0;

    while (start < view.length &&
           (view.data[start] == ' ' ||
            view.data[start] == '\t' ||
            view.data[start] == '\n'))
    {
        start++;
    }

    return string_view_substr(
        view,
        start,
        view.length - start
    );
}


static inline StringView string_view_trim_right(
    StringView view
)
{
    size_t end = view.length;

    while (end > 0 &&
           (view.data[end - 1] == ' ' ||
            view.data[end - 1] == '\t' ||
            view.data[end - 1] == '\n'))
    {
        end--;
    }

    return string_view_slice(view, 0, end);
}


static inline StringView string_view_trim(
    StringView view
)
{
    return string_view_trim_right(
        string_view_trim_left(view)
    );
}


/* ---------------------------------------------------------
    Copying
--------------------------------------------------------- */

static inline size_t string_view_copy(
    StringView view,
    char *buffer,
    size_t buffer_size
)
{
    if (buffer_size == 0)
        return 0;

    size_t count = view.length;

    if (count >= buffer_size)
        count = buffer_size - 1;

    for (size_t i = 0; i < count; i++)
        buffer[i] = view.data[i];

    buffer[count] = '\0';

    return count;
}

static inline void print_string_view(StringView view)
{
    printf("%.*s", (int)view.length, view.data);
}

static inline void print_string_view_ln(StringView view)
{
    print_string_view(view);
    printf("\n");
}

static inline void print_string_view_quoted(StringView view)
{
    printf("\"%.*s\"\n", (int)view.length, view.data);
}

static inline void print_string_view_single_quoted(StringView view)
{
    printf("'%.*s'\n", (int)view.length, view.data);
}

#endif // COGLET_STRING_VIEW_H
