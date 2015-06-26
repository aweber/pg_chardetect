/*
pg_chardetect

A PostgreSQL extension to detect the character set of text-based columns.


pg_chardetect uses software from

 - ICU (http://site.icu-project.org/)

Copyright (c) 2014, AWeber Communications.

pg_chardetect is licensed under the PostgreSQL license.

Permission to use, copy, modify, and distribute this software and its
documentation for any purpose, without fee, and without a written agreement
is hereby granted, provided that the above copyright notice and this paragraph
and the following two paragraphs appear in all copies.

IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE TO ANY
PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES,
INCLUDING LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
DOCUMENTATION, EVEN IF AWEBER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
DAMAGE.

AWEBER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS" BASIS, AND AWEBER HAS
NO OBLIGATIONS TO PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
MODIFICATIONS.

*/


#include "postgres.h"
#include "stdio.h"
#include <string.h>
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "access/heapam.h"
#include "funcapi.h"

#include "unicode/utypes.h"
#include "unicode/ucsdet.h"
#include "unicode/ucnv.h"
#include "unicode/ucnv_err.h"
#include "unicode/ustring.h"
#include "unicode/uloc.h"
//#include "unicode/unistr.h"

#include "flagcb.h"

PG_MODULE_MAGIC;

// Forward declarations

Datum       char_set_detect(PG_FUNCTION_ARGS);
Datum       convert_to_UTF8(PG_FUNCTION_ARGS);

UErrorCode  detect_ICU(const text* buffer, text** encoding, text** lang, int32_t* confidence);

// UErrorCode  force_conversion(const char* cbuffer, const text* encoding, char** converted_buf, int32_t* converted_len);
char* strip_bytes(const char* buffer, int32_t buffer_len, const char* bad_bytes, int8_t bad_bytes_len);

UErrorCode  convert_to_unicode(const text* buffer, const text* encoding, UChar** uBuf, int32_t *uBuf_len, bool force, bool* dropped_bytes);
UErrorCode  convert_to_utf8(const UChar* buffer, int32_t buffer_len, char** converted_buf, int32_t *converted_buf_len, bool force, bool* dropped_bytes);

#define STRING_IS_NULL_TERMINATED -1

/*
Functions:

exposed:

    char_set_detect(text):
        - input is text to convert
        - returns encoding, language, confidence (0-100)

    convert_to_UTF8(text, boolean):
        - input is text to convert,
          true to force conversion by dropping bytes,
          false to not force conversion
        - returns converted text if successful, input text if not,
          converted boolean flag, dropped_bytes boolean flag

internal:

    detect_ICU()

*/

UErrorCode
detect_ICU(const text* buffer, text** encoding, text** lang, int32_t* confidence)
{
    const char* cbuffer = text_to_cstring(buffer);
    //int cbuffer_len = strlen(cbuffer);

    UCharsetDetector* csd;
    const UCharsetMatch* csm;
    UErrorCode status = U_ZERO_ERROR;

    csd = ucsdet_open(&status);

    // set text buffer
    // use -1 for string length since NUL terminated
    ucsdet_setText(csd, cbuffer, STRING_IS_NULL_TERMINATED, &status);
    //ucsdet_setText(csd, cbuffer, cbuffer_len, &status);

    // detect charset
    csm = ucsdet_detect(csd, &status);

    // charset match is NULL if no match
    if (NULL == csm)
    {
        ereport(WARNING,
            (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
             errmsg("ICU error: No charset match for \"%s\" - assuming ISO-8859-1.", cbuffer)));

        *encoding = cstring_to_text("ISO-8859-1");
        *lang = NULL;
        *confidence = 0;

        ucsdet_close(csd);
        pfree((void *) cbuffer);
        return status;
    }
    else if (U_FAILURE(status))
    {
        ereport(WARNING,
            (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
             errmsg("ICU error: %s\n", u_errorName(status))));

        *encoding = NULL;
        *lang = NULL;
        *confidence = 0;

        ucsdet_close(csd);
        pfree((void *) cbuffer);
        return status;
    }

    *encoding = cstring_to_text(ucsdet_getName(csm, &status));
    *lang = cstring_to_text(ucsdet_getLanguage(csm, &status));
    *confidence = ucsdet_getConfidence(csm, &status);

    // close charset detector
    // UCharsetMatch is owned by UCharsetDetector so its memory will be
    // freed when the char set detector is closed
    ucsdet_close(csd);
    pfree((void *) cbuffer);
    return status;
}

UErrorCode
convert_to_unicode(const text* buffer, const text* encoding, UChar** uBuf, int32_t *uBuf_len, bool force, bool* dropped_bytes)
{
    UErrorCode status = U_ZERO_ERROR;

    UConverter *conv;
    int32_t uConvertedLen = 0;

    // used to set dropped_bytes flag if force is true
    ToUFLAGContext * context = NULL;

    size_t uBufSize = 0;

    const char* encoding_cstr = text_to_cstring(encoding);

    // open converter for detected encoding
    conv = ucnv_open(encoding_cstr, &status);

    if (U_FAILURE(status))
    {
        ereport(WARNING,
            (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
             errmsg("Cannot open %s converter - error: %s.\n", (const char *) encoding_cstr, u_errorName(status))));

        if (NULL != encoding_cstr)
            pfree((void *) encoding_cstr);

        ucnv_close(conv);
        return status;
    }

    if (force)
    {
        // set callback to skip illegal, irregular or unassigned bytes

        // set converter to use SKIP callback
        // contecxt will save and call it after calling custom callback
        ucnv_setToUCallBack(conv, UCNV_TO_U_CALLBACK_SKIP, NULL, NULL, NULL, &status);

        //TODO: refactor warning and error message reporting
        if (U_FAILURE(status))
        {
            ereport(WARNING,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("Cannot set callback on converter - error: %s.\n", u_errorName(status))));

            if (NULL != encoding_cstr)
                pfree((void *) encoding_cstr);

            ucnv_close(conv);
            return status;
        }

        // initialize flagging callback
        context = flagCB_toU_openContext();

        /* Set our special callback */
        ucnv_setToUCallBack(conv,
                            flagCB_toU,
                            context,
                            &(context->subCallback),
                            &(context->subContext),
                            &status
                           );

        if (U_FAILURE(status))
        {
            ereport(WARNING,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("Cannot set callback on converter - error: %s.\n", u_errorName(status))));

            if (NULL != encoding_cstr)
                pfree((void *) encoding_cstr);

            ucnv_close(conv);
            return status;
        }
    }

    // allocate unicode buffer
    // must pfree before exiting calling function
    uBufSize = (VARSIZE_ANY_EXHDR(buffer)/ucnv_getMinCharSize(conv) + 1);
    *uBuf = (UChar*) palloc0(uBufSize * sizeof(UChar));

    if (*uBuf == NULL)
    {
        status = U_MEMORY_ALLOCATION_ERROR;

        ereport(WARNING,
            (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
             errmsg("Cannot allocate %d bytes for Unicode pivot buffer - error: %s.\n", (int) uBufSize, u_errorName(status))));

        if (NULL != encoding_cstr)
            pfree((void *) encoding_cstr);

        ucnv_close(conv);
        return status;
    }

    ereport(DEBUG1,
        (errcode(ERRCODE_SUCCESSFUL_COMPLETION),
            errmsg("Original string: %s\n", (const char*) text_to_cstring(buffer))));

    // convert to Unicode
    // returns length of converted string, not counting NUL-terminator
    uConvertedLen = ucnv_toUChars(conv,
                                  *uBuf,
                                  uBufSize,
                                  (const char*) text_to_cstring(buffer),
                                  STRING_IS_NULL_TERMINATED,
                                  &status
                                 );

    if (U_SUCCESS(status))
    {
        // add 1 for NUL terminator
        *uBuf_len = uConvertedLen + 1;

        ereport(DEBUG1,
            (errcode(ERRCODE_SUCCESSFUL_COMPLETION),
                errmsg("Converted string: %s\n", (const char*) *uBuf)));

        // see if any bytes where dropped
        // context struct will go away with converter is closed
        if (NULL != context)
            *dropped_bytes = context->flag;
        else
            *dropped_bytes = false;
    }

    if (U_FAILURE(status))
    {
        ereport(WARNING,
            (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
             errmsg("ICU conversion from %s to Unicode failed - error: %s.\n", encoding_cstr, u_errorName(status))));
    }

    if (NULL != encoding_cstr)
        pfree((void *) encoding_cstr);

    ucnv_close(conv);
    return status;
}


UErrorCode
convert_to_utf8(const UChar* buffer, int32_t buffer_len, char** converted_buf, int32_t *converted_buf_len, bool force, bool* dropped_bytes)
{
    UErrorCode status = U_ZERO_ERROR;
    UConverter *conv;
    int32_t utfConvertedLen = 0;

    // used to set dropped_bytes flag if force is true
    FromUFLAGContext * context = NULL;

    // open UTF8 converter
    conv = ucnv_open("utf-8", &status);

    if (U_FAILURE(status))
    {
        ereport(WARNING,
            (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
             errmsg("Cannot open utf-8 converter - error: %s.\n", u_errorName(status))));

        ucnv_close(conv);
        return status;
    }

    if (force)
    {
        // set callback to skip illegal, irregular or unassigned bytes

        // set converter to use SKIP callback
        // contecxt will save and call it after calling custom callback
        ucnv_setFromUCallBack(conv, UCNV_FROM_U_CALLBACK_SKIP, NULL, NULL, NULL, &status);

        //TODO: refactor warning and error message reporting
        if (U_FAILURE(status))
        {
            ereport(WARNING,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("Cannot set callback on converter - error: %s.\n", u_errorName(status))));

            ucnv_close(conv);
            return status;
        }

        // initialize flagging callback
        context = flagCB_fromU_openContext();

        /* Set our special callback */
        ucnv_setFromUCallBack(conv,
                              flagCB_fromU,
                              context,
                              &(context->subCallback),
                              &(context->subContext),
                              &status
                             );

        if (U_FAILURE(status))
        {
            ereport(WARNING,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("Cannot set callback on converter - error: %s.\n", u_errorName(status))));

           ucnv_close(conv);
            return status;
        }
    }

    // convert to UTF8
    // input buffer from ucnv_toUChars, which always returns a
    // NUL-terminated buffer
    utfConvertedLen = ucnv_fromUChars(conv,
                                      *converted_buf,
                                      *converted_buf_len,
                                      buffer,
                                      STRING_IS_NULL_TERMINATED,
                                      &status
                                     );

    if (U_SUCCESS(status))
    {
        *converted_buf_len = utfConvertedLen;

        ereport(DEBUG1,
            (errcode(ERRCODE_SUCCESSFUL_COMPLETION),
                errmsg("Converted string: %s\n", (const char*) *converted_buf)));

        // see if any bytes where dropped
        // context struct will go away when converter is closed
        if (NULL != context)
            *dropped_bytes = context->flag;
        else
            *dropped_bytes = false;
    }

    if (U_FAILURE(status))
    {
        ereport(WARNING,
            (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
             errmsg("ICU conversion from Unicode to UTF8 failed - error: %s.\n", u_errorName(status))));
    }

    // close the converter
    ucnv_close(conv);
    return status;
}

/*
CREATE OR REPLACE FUNCTION public.convert_to_UTF8
(
IN  text_in text,
IN  force   boolean,
OUT text_out text,
OUT converted boolean
)
AS 'MODULE_PATHNAME', 'char_set_detect'
LANGUAGE C STRICT;
*/

/* by reference, variable length */

PG_FUNCTION_INFO_V1(convert_to_UTF8);

Datum
convert_to_UTF8(PG_FUNCTION_ARGS)
{
    // things we need to deal with constructing our composite type
    TupleDesc   tupdesc;
    Datum       values[3];
    bool        nulls[3];
    HeapTuple   tuple;

    // for char_set_detect function returns
    text    *encoding = NULL;
    text    *lang = NULL;
    int32_t confidence = 0;

    UErrorCode status = U_ZERO_ERROR;

    // output buffer for conversion to Unicode
    UChar* uBuf = NULL;
    int32_t uBuf_len = 0;

    // output of this function
    text *text_out;
    bool converted = false;
    bool dropped_bytes = false;
    bool dropped_bytes_toU = false;
    bool dropped_bytes_fromU = false;

    // temporary buffer for converted string
    char* converted_buf = NULL;

    // input args
    const text  *buffer = PG_GETARG_TEXT_P(0);
    const bool  force   = PG_GETARG_BOOL(1);

    // C string of text* buffer
    const char* cbuffer = NULL;
    int cbuffer_len = 0;

    // Convert output values into a PostgreSQL composite type.
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
              errmsg("function returning record called in context "
                     "that cannot accept type record.\n")));

    // BlessTupleDesc for Datums
    BlessTupleDesc(tupdesc);

    // return if string to convert is NULL
    if (NULL == buffer)
    {
        // return input string,
        // converted to true,
        // dropped_bytes to false
        text_out = (text *) buffer;
        converted = true;
        dropped_bytes = false;
    }
    else
    {
        // extract string from text* to C string
        cbuffer = text_to_cstring(buffer);
        cbuffer_len = strlen(cbuffer);

        // bail on zero-length strings
        // return if cbuffer has zero length or contains a blank space
        if ((0 == cbuffer_len) || (0 == strcmp("", cbuffer)))
        {
            text_out = (text *) buffer;
            converted = true;
            dropped_bytes = false;
        }
        else
        {
            // UTF8 output can be up to 6 bytes per input byte
            // palloc0 allocates and zeros bytes in array
            int32_t converted_buf_len = cbuffer_len * 6 * sizeof(char);
            converted_buf = (char *) palloc0(converted_buf_len);
            // int32_t converted_len = 0;

            // detect encoding with ICU
            status = detect_ICU(buffer, &encoding, &lang, &confidence);

            ereport(DEBUG1,
                (errcode(ERRCODE_SUCCESSFUL_COMPLETION),
                 errmsg("ICU detection status: %d\n", status)));

            ereport(DEBUG1,
                (errcode(ERRCODE_SUCCESSFUL_COMPLETION),
                 errmsg("Detected encoding: %s, language: %s, confidence: %d\n",
                 text_to_cstring(encoding),
                 text_to_cstring(lang),
                         confidence)));

            // return without attempting a conversion if UTF8 is detected
            if (
                (0 == strcmp("UTF-8", text_to_cstring(encoding))) ||
                (0 == strcmp("utf-8", text_to_cstring(encoding))) ||
                (0 == strcmp("UTF8", text_to_cstring(encoding)))  ||
                (0 == strcmp("utf8", text_to_cstring(encoding)))
               )
            {
                ereport(DEBUG1,
                    (errcode(ERRCODE_SUCCESSFUL_COMPLETION),
                     errmsg("ICU detected %s.  No conversion necessary.\n", text_to_cstring(encoding))));

                text_out = (text *) buffer;
                converted = true;
                dropped_bytes = false;
            }
            else
            {
                // ICU uses UTF16 internally, so need to convert to Unicode first
                // then convert to UTF8

                if (U_SUCCESS(status))
                    status = convert_to_unicode(buffer, (const text*) encoding, &uBuf, (int32_t*) &uBuf_len, force, &dropped_bytes_toU);

                if (U_SUCCESS(status))
                    status = convert_to_utf8((const UChar*) uBuf, uBuf_len, &converted_buf, (int32_t*) &converted_buf_len, force, &dropped_bytes_fromU);

                if (U_SUCCESS(status))
                {
                    text_out = cstring_to_text(converted_buf);
                    converted = true;
                    dropped_bytes = (dropped_bytes_toU || dropped_bytes_fromU);
                }
                else
                {
                    ereport(WARNING,
                        (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                            errmsg("ICU conversion failed - returning original input")));

                    text_out = (text *) buffer;
                    converted = false;
                    dropped_bytes = false;
                }
            } // already UTF8
        } // zero-length string
    }  // return if buffer is NULL

    values[0] = PointerGetDatum(text_out);
    values[1] = BoolGetDatum(converted);
    values[2] = BoolGetDatum(dropped_bytes);

    // check if pointers are still NULL; if so Datum is NULL and
    // confidence is meaningless (also NULL)
    (text_out  == NULL || ! VARSIZE_ANY_EXHDR(text_out)) ? (nulls[0] = true) : (nulls[0] = false);
    // converted will never be NULL
    nulls[1] = false;
    nulls[2] = false;

    // build tuple from datum array
    tuple = heap_form_tuple(tupdesc, values, nulls);

    // cleanup
    if (NULL != encoding)
        pfree((void *) encoding);

    if (NULL != lang)
        pfree((void *) lang);

    if (NULL != cbuffer)
        pfree((void *) cbuffer);

    if (NULL != converted_buf)
        pfree((void *) converted_buf);

    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/* by reference, variable length */

PG_FUNCTION_INFO_V1(char_set_detect);

Datum
char_set_detect(PG_FUNCTION_ARGS)
{
    // things we need to deal with constructing our composite type
    TupleDesc   tupdesc;
    Datum       values[3];
    bool        nulls[3];
    HeapTuple   tuple;

    text        *encoding = NULL;
    text        *lang = NULL;
    int32_t     confidence = 0;
    UErrorCode  status = U_ZERO_ERROR;

    const text  *buffer = PG_GETARG_TEXT_P(0);

    // Convert this value into a PostgreSQL composite type.

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
              errmsg("function returning record called in context "
                     "that cannot accept type record")));

    // BlessTupleDesc for Datums
    BlessTupleDesc(tupdesc);

    status = detect_ICU(buffer, &encoding, &lang, &confidence);
    ereport(DEBUG1,
        (errcode(ERRCODE_SUCCESSFUL_COMPLETION),
         errmsg("ICU detection status: %d\n", status)));

    ereport(DEBUG1,
        (errcode(ERRCODE_SUCCESSFUL_COMPLETION),
         errmsg("Detected encoding: %s, length: %d\n", text_to_cstring(encoding), VARSIZE_ANY_EXHDR(encoding))));

    values[0] = PointerGetDatum(encoding);
    values[1] = PointerGetDatum(lang);
    values[2] = Int32GetDatum(confidence);

    // check if pointers are still NULL; if so Datum is NULL and
    // confidence is meaningless (also NULL)
    (encoding == NULL || ! VARSIZE_ANY_EXHDR(encoding)) ? (nulls[0] = true) : (nulls[0] = false);
    (lang == NULL || ! VARSIZE_ANY_EXHDR(lang))         ? (nulls[1] = true) : (nulls[1] = false);
    (nulls[0] == true)                                  ? (nulls[2] = true) : (nulls[2] = false);

    // build tuple from datum array
    tuple = heap_form_tuple(tupdesc, values, nulls);

    // cleanup
    encoding ? pfree(encoding) : NULL;
    lang ? pfree(lang) : NULL;

    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}



/*
typedef enum ICU_charsets
{   UTF-8,
    UTF-16BE,
    UTF-16LE,
    UTF-32BE,
    UTF-32LE.
    Shift_JIS,      // Japanese
    ISO-2022-JP,    // Japanese
    ISO-2022-CN,    // Simplified Chinese
    ISO-2022-KR,    // Korean
    GB18030,        // Chinese
    Big5,           // Traditional Chinese
    EUC-JP,         // Japanese
    EUC-KR,         // Korean
    ISO-8859-1,     // Danish, Dutch, English, French, German, Italian, Norwegian, Portuguese, Swedish
    ISO-8859-2,     // Czech, Hungarian, Polish, Romanian
    ISO-8859-5,     // Russian
    ISO-8859-6,     // Arabic
    ISO-8859-7,     // Greek
    ISO-8859-8,     // Hebrew
    ISO-8859-9,     // Turkish
    windows-1250,   // Czech, Hungarian, Polish, Romanian
    windows-1251,   // Russian
    windows-1252,   // Danish, Dutch, English, French, German, Italian, Norwegian, Portuguese, Swedish
    windows-1253,   // Greek
    windows-1254,   // Turkish
    windows-1255,   // Hebrew
    windows-1256,   // Arabic
    KOI8-R,         // Russian
    IBM420,         // Arabic
    IBM424          // Hebrew
 } ICU_charsets;    // PG to ICU name map

pg_enc2gettext pg_enc2gettext_tbl[] =
{
    {PG_UTF8, "UTF-8"},
    {PG_LATIN1, "LATIN1"},
    {PG_LATIN2, "LATIN2"},
    {PG_LATIN3, "LATIN3"},
    {PG_LATIN4, "LATIN4"},
    {PG_ISO_8859_5, "ISO-8859-5"},
    {PG_ISO_8859_6, "ISO_8859-6"},
    {PG_ISO_8859_7, "ISO-8859-7"},
    {PG_ISO_8859_8, "ISO-8859-8"},
    {PG_LATIN5, "LATIN5"},
    {PG_LATIN6, "LATIN6"},
    {PG_LATIN7, "LATIN7"},
    {PG_LATIN8, "LATIN8"},
    {PG_LATIN9, "LATIN-9"},
    {PG_LATIN10, "LATIN10"},
    {PG_KOI8R, "KOI8-R"},
    {PG_KOI8U, "KOI8-U"},
    {PG_WIN1250, "CP1250"},
    {PG_WIN1251, "CP1251"},
    {PG_WIN1252, "CP1252"},
    {PG_WIN1253, "CP1253"},
    {PG_WIN1254, "CP1254"},
    {PG_WIN1255, "CP1255"},
    {PG_WIN1256, "CP1256"},
    {PG_WIN1257, "CP1257"},
    {PG_WIN1258, "CP1258"},
    {PG_WIN866, "CP866"},
    {PG_WIN874, "CP874"},
    {PG_EUC_CN, "EUC-CN"},
    {PG_EUC_JP, "EUC-JP"},
    {PG_EUC_KR, "EUC-KR"},
    {PG_EUC_TW, "EUC-TW"},
    {PG_EUC_JIS_2004, "EUC-JP"},
    {0, NULL}
};

typedef struct pg2ICU
{
    const char *pg_enc_name;
    const char *ICU_enc_name;
} pg2ICU;

pg2ICU pg2ICU_map[] =
{
    {"UTF-8",       "UTF-8"},
    {"BIG5",        "Big5"},
    {"LATIN1",      "ISO-8859-1"},
    {"LATIN2",      "ISO-8859-2"},
    {"LATIN3",      "ISO-8859-3"},
    {"LATIN4",      "ISO-8859-4"},
    {"ISO-8859-5",  "ISO-8859-5"},
    {"ISO_8859-6",  "ISO-8859-6"},
    {"ISO-8859-7",  "ISO-8859-7"},
    {"ISO-8859-8",  "ISO-8859-8"},
    {"LATIN5",      "ISO-8859-5"},
    {"LATIN6",      "ISO-8859-6"},
    {"LATIN7",      "ISO-8859-7"},
    {"LATIN8",      "ISO-8859-8"},
    {"LATIN-9",     "ISO-8859-9"},
    {"LATIN10",     "ISO-8859-16"},
    {"KOI8-R",      "KOI8-R"},
    {"KOI8-U",      ""},
    {"CP1250",      "windows-1250"},
    {"CP1251",      "windows-1251"},
    {"CP1252",      "windows-1252"},
    {"CP1253",      "windows-1253"},
    {"CP1254",      "windows-1254"},
    {"CP1255",      "windows-1255"},
    {"CP1256",      "windows-1256"},
    {"CP1257",      ""},
    {"CP1258",      ""},
    {"CP866",       ""},
    {"CP874",       ""},
    {"EUC-CN",      "ISO-2022-CN"},
    {"EUC-JP",      "ISO-2022-JP"},
    {"EUC-KR",      "ISO-2022-KR"},
    {"EUC-TW",      ""},
    {"EUC-JP",      "Shift_JIS"}
};
*/
