#include "stdafx.h"

#include "xrstring.h"

XRCORE_API str_container* g_pStringContainer{};

struct str_container_impl
{
    xrCriticalSection cs;
    static constexpr size_t buffer_size = 1024u * 256u;
    str_value* buffer[buffer_size]{};
    int num_docs{};

    str_container_impl() = default;

    str_value* find(const str_value* value, const char* str) const
    {
        str_value* candidate = buffer[value->dwCRC % buffer_size];
        while (candidate)
        {
            if (candidate->dwCRC == value->dwCRC && candidate->dwLength == value->dwLength && !memcmp(candidate->value, str, value->dwLength))
            {
                return candidate;
            }

            candidate = candidate->next;
        }

        return nullptr;
    }

    void insert(str_value* value)
    {
        str_value** element = &buffer[value->dwCRC % buffer_size];
        value->next = *element;
        *element = value;
    }

    void clean()
    {
        for (auto& i : buffer)
        {
            str_value** current = &i;

            while (*current != nullptr)
            {
                str_value* value = *current;
                if (!value->dwReference)
                {
                    *current = value->next;
                    xr_free(value);
                }
                else
                {
                    current = &value->next;
                }
            }
        }
    }

    ptrdiff_t stat_economy() const
    {
        ptrdiff_t counter = 0;
        for (auto value : buffer)
        {
            while (value)
            {
                counter -= sizeof(str_value);
                counter += (value->dwReference - 1) * (value->dwLength + 1);
                value = value->next;
            }
        }

        return counter;
    }
};

str_container::str_container() : impl(xr_new<str_container_impl>()) {}

str_value* str_container::dock(pcstr value) const
{
    if (nullptr == value)
        return nullptr;

    impl->cs.Enter();

    // calc len
    const auto s_len = xr_strlen(value);
    const auto s_len_with_zero = s_len + 1;
    VERIFY(sizeof(str_value) + s_len_with_zero < 4096);

    // setup find structure
    char header[sizeof(str_value)];
    str_value* sv = (str_value*)header;
    sv->dwReference = 0;
    sv->dwLength = static_cast<u32>(s_len);
    sv->dwCRC = crc32(value, s_len);

    // search
    str_value* result = impl->find(sv, value);

    // it may be the case, string is not found or has "non-exact" match
    if (nullptr == result)
    {
        result = static_cast<str_value*>(xr_malloc(sizeof(str_value) + s_len_with_zero));

        result->dwReference = 0;
        result->dwLength = sv->dwLength;
        result->dwCRC = sv->dwCRC;
        CopyMemory(result->value, value, s_len_with_zero);

        impl->insert(result);
    }
    impl->cs.Leave();

    return result;
}

void str_container::clean() const
{
    impl->cs.Enter();
    impl->clean();
    impl->cs.Leave();
}

size_t str_container::stat_economy() const
{
    impl->cs.Enter();
    ptrdiff_t counter = 0;
    counter -= sizeof(*this);
    counter += impl->stat_economy();
    impl->cs.Leave();
    return size_t(counter);
}

str_container::~str_container()
{
    clean();
}
