/*
*   Copyright (c) 1998-2002, Darren Hiebert
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License version 2 or (at your option) any later version.
*
*   External interface to entry.c
*/

#include "general.h"  /* must always come first */

#include "entry_p.h"
#include "field.h"
#include "field_p.h"
#include "mio.h"
#include "options_p.h"
#include "parse_p.h"
#include "ptag_p.h"
#include "read.h"
#include "writer_p.h"


#define CTAGS_FILE  "tags"


static int writeCtagsEntry (tagWriter *writer CTAGS_ATTR_UNUSED,
							MIO * mio, const tagEntryInfo *const tag);
static int writeCtagsPtagEntry (tagWriter *writer CTAGS_ATTR_UNUSED,
								MIO * mio, const ptagDesc *desc,
								const char *const fileName,
								const char *const pattern,
								const char *const parserName);
static bool treatFieldAsFixed (int fieldType);

struct rejection {
	bool rejectionInThisInput;
};

tagWriter uCtagsWriter = {
	.writeEntry = writeCtagsEntry,
	.writePtagEntry = writeCtagsPtagEntry,
	.preWriteEntry = NULL,
	.postWriteEntry = NULL,
	.treatFieldAsFixed = treatFieldAsFixed,
	.defaultFileName = CTAGS_FILE,
};

static void *beginECtagsFile (tagWriter *writer CTAGS_ATTR_UNUSED, MIO * mio CTAGS_ATTR_UNUSED)
{
	static struct rejection rej;

	rej.rejectionInThisInput = false;

	return &rej;
}

static bool endECTagsFile (tagWriter *writer, MIO * mio CTAGS_ATTR_UNUSED, const char* filename CTAGS_ATTR_UNUSED)
{
	struct rejection *rej = writer->private;
	return rej->rejectionInThisInput;
}

tagWriter eCtagsWriter = {
	.writeEntry = writeCtagsEntry,
	.writePtagEntry = writeCtagsPtagEntry,
	.preWriteEntry = beginECtagsFile,
	.postWriteEntry = endECTagsFile,
	.treatFieldAsFixed = treatFieldAsFixed,
	.defaultFileName = CTAGS_FILE,
};

static bool hasTagEntryTabChar (const tagEntryInfo * const tag)
{

	if (doesFieldHaveTabChar (FIELD_NAME, tag, NO_PARSER_FIELD)
		|| doesFieldHaveTabChar (FIELD_INPUT_FILE, tag, NO_PARSER_FIELD))
		return true;

	if (tag->lineNumberEntry)
	{
		if (Option.lineDirectives)
		{
			if (doesFieldHaveTabChar (FIELD_LINE_NUMBER, tag, NO_PARSER_FIELD))
				return true;
		}
	}
	else if (doesFieldHaveTabChar (FIELD_PATTERN, tag, NO_PARSER_FIELD))
	{
		/* Pattern may have a tab char. However, doesFieldHaveTabChar returns
		 * false because NO_PARSER_FIELD may not have hasTabChar handler.
		 */
		return true;
	}

	if (includeExtensionFlags ())
	{
		if (isFieldEnabled (FIELD_SCOPE) && doesFieldHaveValue (FIELD_SCOPE, tag)
			&& (doesFieldHaveTabChar (FIELD_SCOPE_KIND_LONG, tag, NO_PARSER_FIELD)
				|| doesFieldHaveTabChar (FIELD_SCOPE, tag, NO_PARSER_FIELD)))
			return true;
		if (isFieldEnabled (FIELD_TYPE_REF) && doesFieldHaveValue (FIELD_TYPE_REF, tag)
			&& doesFieldHaveTabChar (FIELD_TYPE_REF, tag, NO_PARSER_FIELD))
			return true;
		if (isFieldEnabled (FIELD_FILE_SCOPE) && doesFieldHaveValue (FIELD_FILE_SCOPE, tag)
			&& doesFieldHaveTabChar (FIELD_FILE_SCOPE, tag, NO_PARSER_FIELD))
			return true;

		int f[] = { FIELD_INHERITANCE,
					FIELD_ACCESS,
					FIELD_IMPLEMENTATION,
					FIELD_SIGNATURE,
					FIELD_ROLES,
					FIELD_EXTRAS,
					FIELD_XPATH,
					FIELD_END_LINE,
					-1};
		for (unsigned int i = 0; f[i] >= 0; i++)
		{
			if (isFieldEnabled (f[i]) && doesFieldHaveValue (f[i], tag)
				&& doesFieldHaveTabChar (f[i], tag, NO_PARSER_FIELD))
				return true;
		}
	}

	for (unsigned int i = 0; i < tag->usedParserFields; i++)
	{
		const tagField *f = getParserField(tag, i);
		fieldType ftype = f->ftype;
		if (isFieldEnabled (ftype))
		{
			if (doesFieldHaveTabChar (ftype, tag, i))
				return true;
		}
	}
	return false;
}


static const char* escapeFieldValueFull (tagWriter *writer, const tagEntryInfo * tag, fieldType ftype, int fieldIndex)
{
	const char *v;
	if (writer->type == WRITER_E_CTAGS && doesFieldHaveRenderer(ftype, true))
		v = renderFieldNoEscaping (ftype, tag, fieldIndex);
	else
		v = renderField (ftype, tag, fieldIndex);

	return v;
}

static const char* escapeFieldValue (tagWriter *writer, const tagEntryInfo * tag, fieldType ftype)
{
	return escapeFieldValueFull (writer, tag, ftype, NO_PARSER_FIELD);
}

static int renderExtensionFieldMaybe (tagWriter *writer, int xftype, const tagEntryInfo *const tag, char sep[2], MIO *mio)
{
	if (isFieldEnabled (xftype) && doesFieldHaveValue (xftype, tag))
	{
		int len;
		len = mio_printf (mio, "%s\t%s:%s", sep,
				  getFieldName (xftype),
				  escapeFieldValue (writer, tag, xftype));
		sep[0] = '\0';
		return len;
	}
	else
		return 0;
}

static int addParserFields (tagWriter *writer, MIO * mio, const tagEntryInfo *const tag)
{
	unsigned int i;
	int length = 0;

	for (i = 0; i < tag->usedParserFields; i++)
	{
		const tagField *f = getParserField(tag, i);
		fieldType ftype = f->ftype;
		if (! isFieldEnabled (ftype))
			continue;

		length += mio_printf(mio, "\t%s:%s",
							 getFieldName (ftype),
							 escapeFieldValueFull (writer, tag, ftype, i));
	}
	return length;
}

static int writeLineNumberEntry (tagWriter *writer, MIO * mio, const tagEntryInfo *const tag)
{
	if (Option.lineDirectives)
		return mio_printf (mio, "%s", escapeFieldValue (writer, tag, FIELD_LINE_NUMBER));
	else
		return mio_printf (mio, "%lu", tag->lineNumber);
}

static int addExtensionFields (tagWriter *writer, MIO *mio, const tagEntryInfo *const tag)
{
	bool isKindKeyEnabled = isFieldEnabled (FIELD_KIND_KEY);
	bool isScopeEnabled = isFieldEnabled   (FIELD_SCOPE_KEY);

	const char* const kindKey = isKindKeyEnabled
		?getFieldName (FIELD_KIND_KEY)
		:"";
	const char* const kindFmt = isKindKeyEnabled
		?"%s\t%s:%s"
		:"%s\t%s%s";
	const char* const scopeKey = isScopeEnabled
		?getFieldName (FIELD_SCOPE_KEY)
		:"";
	const char* const scopeFmt = isScopeEnabled
		?"%s\t%s:%s:%s"
		:"%s\t%s%s:%s";

	char sep [] = {';', '"', '\0'};
	int length = 0;

	const char *str = NULL;;
	kindDefinition *kdef = getLanguageKind(tag->langType, tag->kindIndex);
	const char kind_letter_str[2] = {kdef->letter, '\0'};

	if (kdef->name != NULL && (isFieldEnabled (FIELD_KIND_LONG)  ||
		 (isFieldEnabled (FIELD_KIND)  && kdef->letter == KIND_NULL)))
	{
		/* Use kind long name */
		str = kdef->name;
	}
	else if (kdef->letter != KIND_NULL  && (isFieldEnabled (FIELD_KIND) ||
			(isFieldEnabled (FIELD_KIND_LONG) &&  kdef->name == NULL)))
	{
		/* Use kind letter */
		str = kind_letter_str;
	}

	if (str)
	{
		length += mio_printf (mio, kindFmt, sep, kindKey, str);
		sep [0] = '\0';
	}

	if (isFieldEnabled (FIELD_LINE_NUMBER) &&  doesFieldHaveValue (FIELD_LINE_NUMBER, tag))
	{
		length += mio_printf (mio, "%s\t%s:%ld", sep,
				   getFieldName (FIELD_LINE_NUMBER),
				   tag->lineNumber);
		sep [0] = '\0';
	}

	length += renderExtensionFieldMaybe (writer, FIELD_LANGUAGE, tag, sep, mio);

	if (isFieldEnabled (FIELD_SCOPE))
	{
		const char* k, *v;

		k = escapeFieldValue (writer, tag, FIELD_SCOPE_KIND_LONG);
		v = escapeFieldValue (writer, tag, FIELD_SCOPE);
		if (k && v)
		{
			length += mio_printf (mio, scopeFmt, sep, scopeKey, k, v);
			sep [0] = '\0';
		}
	}

	if (isFieldEnabled (FIELD_TYPE_REF) && doesFieldHaveValue (FIELD_TYPE_REF, tag))
	{
		length += mio_printf (mio, "%s\t%s:%s", sep,
				      getFieldName (FIELD_TYPE_REF),
				      escapeFieldValue (writer, tag, FIELD_TYPE_REF));
		sep [0] = '\0';
	}

	if (isFieldEnabled (FIELD_FILE_SCOPE) &&  doesFieldHaveValue (FIELD_FILE_SCOPE, tag))
	{
		length += mio_printf (mio, "%s\t%s:", sep,
				      getFieldName (FIELD_FILE_SCOPE));
		sep [0] = '\0';
	}

	length += renderExtensionFieldMaybe (writer, FIELD_INHERITANCE, tag, sep, mio);
	length += renderExtensionFieldMaybe (writer, FIELD_ACCESS, tag, sep, mio);
	length += renderExtensionFieldMaybe (writer, FIELD_IMPLEMENTATION, tag, sep, mio);
	length += renderExtensionFieldMaybe (writer, FIELD_SIGNATURE, tag, sep, mio);
	length += renderExtensionFieldMaybe (writer, FIELD_ROLES, tag, sep, mio);
	length += renderExtensionFieldMaybe (writer, FIELD_EXTRAS, tag, sep, mio);
	length += renderExtensionFieldMaybe (writer, FIELD_XPATH, tag, sep, mio);
	length += renderExtensionFieldMaybe (writer, FIELD_END_LINE, tag, sep, mio);

	return length;
}

static int writeCtagsEntry (tagWriter *writer,
							MIO * mio, const tagEntryInfo *const tag)
{
	if (writer->private)
	{
		struct rejection *rej = writer->private;
		if (hasTagEntryTabChar (tag))
		{
			rej->rejectionInThisInput = true;
			return 0;
		}
	}

	int length = mio_printf (mio, "%s\t%s\t",
			      escapeFieldValue (writer, tag, FIELD_NAME),
			      escapeFieldValue (writer, tag, FIELD_INPUT_FILE));

	/* This is for handling 'common' of 'fortran'.  See the
	   description of --excmd=mixed in ctags.1.  In tags output, what
	   we call "pattern" is instructions for vi.

	   However, in the other formats, pattern should be pattern as its name. */
	if (tag->lineNumberEntry)
		length += writeLineNumberEntry (writer, mio, tag);
	else
	{
		if (Option.locate == EX_COMBINE)
			length += mio_printf(mio, "%lu;", tag->lineNumber + (Option.backward? 1: -1));
		length += mio_puts(mio, escapeFieldValue(writer, tag, FIELD_PATTERN));
	}

	if (includeExtensionFlags ())
	{
		length += addExtensionFields (writer, mio, tag);
		length += addParserFields (writer, mio, tag);
	}

	length += mio_printf (mio, "\n");

	return length;
}

static int writeCtagsPtagEntry (tagWriter *writer CTAGS_ATTR_UNUSED,
				MIO * mio, const ptagDesc *desc,
				const char *const fileName,
				const char *const pattern,
				const char *const parserName)
{
	return parserName

#define OPT(X) ((X)?(X):"")
		? mio_printf (mio, "%s%s%s%s\t%s\t%s\n",
			      PSEUDO_TAG_PREFIX, desc->name, PSEUDO_TAG_SEPARATOR, parserName,
			      OPT(fileName), OPT(pattern))
		: mio_printf (mio, "%s%s\t%s\t/%s/\n",
			      PSEUDO_TAG_PREFIX, desc->name,
			      OPT(fileName), OPT(pattern));
#undef OPT
}

static bool treatFieldAsFixed (int fieldType)
{
	switch (fieldType)
	{
	case FIELD_NAME:
	case FIELD_INPUT_FILE:
	case FIELD_PATTERN:
		return true;
	default:
		return false;
	}
}
