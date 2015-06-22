// Meridian 59, Copyright 1994-2012 Andrew Kirmse and Chris Kirmse.
// All rights reserved.
//
// This software is distributed under a license that is described in
// the LICENSE file that accompanies it.
//
// Meridian is a registered trademark.
/*
 * srvrstr.c:  Deal with strings from the server.
 */

#include "client.h"

/************************************************************************/
/*
 * CheckServerMessage:  Assemble a set of printf-style strings into a resultant
 *   string.  The initial format string is given by the fmt_id resource, and the
 *   parameters are in params.  The result is placed in message.  Len is the # of bytes
 *   in params.
 *   Returns True iff len bytes of parameters are used in assembling message, otherwise
 *   False, which indicates an error in the message from the server.
 *   The allowed printf-style format characters are:
 *   %d or %i    a literal integer
 *   %q          a literal string
 *   %s          an integer which specifies a string resource.  This resource may
 *               contain other format characters.  If so, they are matched with 
 *               parameters from params AFTER the initial string's parameters.
 *   %r          specifies that the next resource in the message should be concatenated
 *               into the place of this format character (calls CheckServerMessage)
 */
Bool CheckServerMessage(char** msg, char **params, long len, ID fmt_id)
{
   char *fmt, *next_ptr; /* next_ptr points into format string fmt */
   char tempfmt[MAXMESSAGE], format[MAXMESSAGE], *param_ptr = *params;
   char *new_param_ptr;
   char* message;
   char message2[MAXMESSAGE];
   char* msg2 = message2; /* Pointer to message2 */
   char *rsc, type_char, *orig_message, *walkrsc;
   DWORD field, num_chars;
   WORD string_len;
   Bool done = False;
   int fieldPos = 0, currentPos = 0;
   char digit[1];
   PosArray posArray;
   memset(&posArray, 0, sizeof posArray);

   /* qparams are %q parameters; we need to save their positions and replace them last, 
    * even after %s (in case replacement %q string contains a literal %s) */
   /* Each element of the qparams array points to the place in params where the string
      for to replace the corresponding %q begins */
   char *qparams[MAXQPARAMS + 1];
   int num_qparams = 1;  /* Start counting at 1 to avoid null character */

   /* Get the buffer to send back the message.
    * If it turns out we have no formatting to do, then we'll send back
    * the resource string itself instead of filling the caller's limited buffer.
    */
   if (!msg || !*msg)
      return False;

   // Set the first character of msg to null terminator, so we can check
   // the length of msg.
   *msg[0] = '\0';

   message = *msg;

   /* Get format string from resources */
   fmt = LookupRsc(fmt_id);
   if (fmt == NULL)
      return False;

   // This section reorders the parameters if the resource uses the
   // $1, $2 etc formatters in conjunction with the string formatters.
   // Can have up to 25 of these.
   rsc = fmt;
   // TODO: not sure if allocating memory is the best way to do this.
   new_param_ptr = (char*)SafeMalloc(len);
   // Copy the parameter pointer into the new memory.
   memcpy(new_param_ptr, param_ptr, len);
   // Used to keep track of the previous character while walking the rsc.
   walkrsc = rsc;
   // Iterate over the resource.
   while (*rsc)
   {
      if (rsc[0] == '%')
      {
         if (rsc[1] != '%')
         {
            // Keep track of the string formatter character.
            walkrsc = rsc;
         }
      }

      if (rsc[0] == '$' && rsc[1] != '$')
      {
         // Get the numbers for field position.
         if (rsc[1] >= '0' && rsc[1] <= '9')
         digit[0] = rsc[1];
         if (rsc[2] >= '0' && rsc[2] <= '9')
            digit[1] = rsc[2];
         // Take one, because arrays are 0-indexed.
         fieldPos = atoi(digit) - 1;
         if (walkrsc[0] == '%')
         {
            switch (walkrsc[1])
            {
               case 'r':
                  // Can't use numbered parameters with the 'r' string formatter.
                  SafeFree(new_param_ptr);
                  return False;
               case 'i':
               case 's':
               case 'd':
                  // Increment new_param_ptr to the next field.
                  new_param_ptr += SIZE_ID;
                  // Store the new position for this formatter.
                  posArray.fieldPos[currentPos] = fieldPos;
                  // Store the size of the current field.
                  posArray.size[currentPos] = SIZE_ID;
                  // Keep track of the running size total for each position.
                  // Offset by 1 position (because pos2 starts at end of pos1, etc).
                  for (int i = currentPos + 1; i < MAXQPARAMS; i++)
                     posArray.bytePos[i] += SIZE_ID;
                  break;
               case 'q':
                  // Get the length of the string.
                  memcpy(&string_len, new_param_ptr, SIZE_STRING_LEN);
                  // Increment new_param_ptr to the next field.
                  new_param_ptr += string_len + SIZE_STRING_LEN;
                  // Store the new position for this formatter.
                  posArray.fieldPos[currentPos] = fieldPos;
                  // Store the size of the current field.
                  posArray.size[currentPos] = string_len + SIZE_STRING_LEN;
                  // Keep track of the running size total for each position.
                  for (int i = currentPos; i < MAXQPARAMS; i++)
                     posArray.bytePos[i] += string_len + SIZE_STRING_LEN;
                  break;
               default:
                  break;
            }
            currentPos++;
         }
         rsc++;
      }
      else if (rsc[0] == '$' && rsc[1] == '$')
      {
         rsc+= 2;
      }
      else
      {
         rsc++;
      }
   }
   if (currentPos > 0)
   {
      // Decrement new_param_ptr back to the start.
      new_param_ptr -= posArray.bytePos[currentPos];
      for (int i = 0; i < currentPos; i++)
      {
         // Copy the data at the current position to the requested field position,
         // using the byte position stored at that field position. Use two ptrs
         // here because there's no guarantee that fields are all the same size.
         memcpy(param_ptr + posArray.bytePos[posArray.fieldPos[i]], new_param_ptr, posArray.size[i]);
         new_param_ptr += posArray.size[i];
      }
   }
   SafeFree(new_param_ptr);

   /* Is there anything to format at all?
    * Or can we return the "format" resource as-is?
    */
   rsc = fmt;
   while (*rsc)
   {
      if (rsc[0] == '%')
      {
         if (rsc[1] == '%')
            rsc++;
         else
            break;
      }

      rsc++;
   }

   if (!*rsc)
   {
      *msg = fmt;

      return True;
   }

   /* Prepare to format into the caller's message buffer. */
   orig_message = message;

   /* Keep looping through string until there's nothing left to replace */
   while (!done)
   {
      done = True;  /* We'll be done if we don't find any %s's */

      /* Find first format field */
      next_ptr = strchr(fmt, '%');
      
      /* Invariant:  len is # of bytes remaining in params */
      while (next_ptr != NULL)
      {
         next_ptr++;  /* Move to type character */

         type_char = *next_ptr;
         /* If string ends with %, done */
         if (type_char == '\0')
            break;

         /* Skip marked %q parameters */
         if (type_char <= MAXQPARAMS)
         {
            next_ptr = strchr(next_ptr, '%');

            continue;
         }

         next_ptr++;  /* Move past field specification char */

         /* Make temporary buffer for this section of format string */
         strncpy(tempfmt, fmt, next_ptr - fmt);
         tempfmt[next_ptr - fmt] = '\0';

         switch (type_char)
         {
         case '%':              /* %% ==> % */
            *message++ = '%';
            break;
         case 'r':
            // Get the next resource in the server message, increment param_ptr.
            memcpy(&field, param_ptr, SIZE_ID);
            param_ptr += SIZE_ID;
            len -= SIZE_ID;

            // Process the next resource as if it were a complete message.
            if (!CheckServerMessage(&msg2, &param_ptr, len, field))
               return False;

            // Check if we're going to write outside the bounds of msg.
            if (strlen(*msg) + strlen(msg2) >= MAXMESSAGE)
               return False;

            // Copy the msg2 string into message.
            num_chars = sprintf(message, "%s", msg2);
            message += num_chars;

            // Reset message2 and msg2.
            message2[MAXMESSAGE];
            msg2 = message2;
            break;
         case 'd':
         case 'i':
         case 's':
            /* See if there are enough bytes left */
            if (len < SIZE_ID)
               return False;

            /* Interpret next field as an integer */
            memcpy(&field, param_ptr, SIZE_ID);
            param_ptr += SIZE_ID;
            len -= SIZE_ID;

            /* Look up resource strings; use integers immediately */
            if (type_char == 's')
            {
               done = False;
               if ((rsc = LookupRsc(field)) == NULL)
                  return False;

               num_chars = sprintf(message, tempfmt, rsc);
            }
            else
            {
               num_chars = sprintf(message, tempfmt, field);
            }

            // Get rid of any numbered parameter formatters.
            if (*next_ptr == '$')
            {
               next_ptr++;
               if (*next_ptr != '$')
               {
                  next_ptr++;
               }
            }

            message += num_chars;  /* Overwrite null char next time */
            break;
         case 'q':     /* Literal string from server */
            /* Store location; we will perform replacement later */	    
            if (len < SIZE_STRING_LEN)
               return False;

            /* We can only hold a certain # of qparams */
            if (num_qparams <= MAXQPARAMS)
            {
               /* Save current location in parameters */
               qparams[num_qparams] = param_ptr;
            }
            memcpy(&string_len, param_ptr, SIZE_STRING_LEN);
            param_ptr += SIZE_STRING_LEN;
            len -= SIZE_STRING_LEN;
            if (len < string_len)
               return False;

            /* Mark this position with qparam # */
            if (num_qparams <= MAXQPARAMS)
            {
               tempfmt[(next_ptr - fmt) - 1] = (char) num_qparams;
               num_qparams++;
            }

            /* Copy this section of format string */
            strncpy(message, tempfmt, (next_ptr - fmt));
            message += (next_ptr - fmt);

            /* Skip string */
            param_ptr += string_len;
            len -= string_len;

            // Get rid of any numbered parameter formatters.
            if (*next_ptr == '$')
            {
               next_ptr++;
               if (*next_ptr != '$')
               {
                  next_ptr++;
               }
            }
            break;
         }

         /* Find next format field */
         fmt = next_ptr;
         next_ptr = strchr(fmt, '%');
      }
      
      /* Copy over last part of string */
      strcpy(message, fmt);

      /* Prepare for next pass */
      message = orig_message;
      strcpy(format, message);
      fmt = format;
   }

   // Set params to remainder of server message
   *params = param_ptr;

   /* Now fill in marked %q fields.  Note that format cotains copy of message */
   fmt = format;
   next_ptr = strchr(format, '%');
   while (next_ptr != NULL)
   {
      next_ptr++;  /* Move to type character */

      type_char = *next_ptr;
      /* If string ends with %, done */
      if (type_char == '\0')
         break;

      /* See if this is a marked %q field */
      if (type_char > MAXQPARAMS)
      {
         next_ptr = strchr(next_ptr, '%');

         continue;
      }

      next_ptr++;  /* Move past field specification char */

      /* Make temporary buffer for this section of format string */
      strncpy(tempfmt, fmt, next_ptr - fmt);
      tempfmt[next_ptr - fmt] = '\0';

      /* Get length of string */
      param_ptr = qparams[type_char];
      memcpy(&string_len, param_ptr, SIZE_STRING_LEN);
      param_ptr += SIZE_STRING_LEN;

      /* Hack off %q from format string */
      tempfmt[(next_ptr - fmt) - 2] = '\0';

      /* Add tempfmt and then literal string to end of message */
      strcpy(message, tempfmt);
      message += strlen(tempfmt);

      strncpy(message, param_ptr, string_len);
      message += string_len;

      fmt = next_ptr;
      next_ptr = strchr(fmt, '%');
   }

   /* Copy over last part of string */
   strcpy(message, fmt);

   return True;
}
/************************************************************************/

static char *format_chars = "`~"; // Characters that start a format code

// FormatCode types
#define CODE_COLOR 0x01
#define CODE_STYLE 0x02

typedef struct {
   char  code;  // Character that formatting type
   BYTE  type;  // Formatting that this code affects (text color, text style)
   int   data;  // Value to set formatting type
} FormatCode;

static FormatCode code_table[] = {
{ 'r', CODE_COLOR, PALETTERGB(128,   0,   0) }, // Dark red
{ 'f', CODE_COLOR, PALETTERGB(200,   0,   0) }, // Bright red
{ 'g', CODE_COLOR, PALETTERGB(  0, 100,   0) }, // Dark green
{ 'l', CODE_COLOR, PALETTERGB(  0, 255,   0) }, // Light green
{ 'b', CODE_COLOR, PALETTERGB(  0,   0, 255) }, // Dark blue
{ 'k', CODE_COLOR, PALETTERGB(  0,   0,   0) }, // Black
{ 'w', CODE_COLOR, PALETTERGB(255, 255, 255) }, // White
{ 'y', CODE_COLOR, PALETTERGB(230, 230,  25) }, // Yellow
{ 'p', CODE_COLOR, PALETTERGB(255, 105, 210) }, // Pink
{ 'o', CODE_COLOR, PALETTERGB(255, 150,   0) }, // Orange
{ 'a', CODE_COLOR, PALETTERGB(127, 255, 212) }, // Aquamarine
{ 'c', CODE_COLOR, PALETTERGB( 46, 234, 250) }, // Cyan
{ 'q', CODE_COLOR, PALETTERGB(143,  38, 170) }, // Purple
{ 't', CODE_COLOR, PALETTERGB( 11,  59, 112) }, // Teal
{ 's', CODE_COLOR, PALETTERGB( 60,  60,  60) }, // Dark grey
{ 'v', CODE_COLOR, PALETTERGB(128,   0, 128) }, // Violet
{ 'm', CODE_COLOR, PALETTERGB(205,   0, 205) }, // Magenta
{ 'B', CODE_STYLE, STYLE_BOLD },
{ 'I', CODE_STYLE, STYLE_ITALIC },
{ 'U', CODE_STYLE, STYLE_UNDERLINE },
{ 'n', CODE_STYLE, STYLE_RESET },
};

static int num_format_codes = sizeof(code_table) / sizeof(FormatCode);

/************************************************************************/
/*
 * DisplayServerMessage:  Display message from server, extracting any style or color codes.
 *   color and style give default style and color values.
 */
void DisplayServerMessage(char *message, COLORREF start_color, BYTE start_style)
{
   EditBoxStartAdd();

   DisplayMessage(message, start_color, start_style);
   
   EditBoxEndAdd();
}
/************************************************************************/
/*
 * DisplayMessage:  Display string, extracting any style or color codes.
 *   color and style give default style and color values.
 *   Does the real work of DisplayServerMessage.
 */
void DisplayMessage(char *message, COLORREF start_color, BYTE start_style)
{
   int i;
   char ch, *ptr, *str;  // str points to start of current piece of message
   COLORREF color, new_color;
   BYTE style, new_style, new_type;
   BOOL bFree = FALSE;
   char* p;

   message = strdup(message);
   p = message;

   if (config.antiprofane)
   {
      if (config.ignoreprofane)
      {
	 if (ContainsProfaneTerms(message))
	 {
 	    message = (char *) SafeMalloc(MAXSTRINGLEN);
	    LoadString(hInst, IDS_PROFANITYREMOVED, message, MAXSTRINGLEN);
	    bFree = TRUE;
	 }
      }
      else
      {
	 message = CleanseProfaneString(message);
	 bFree = TRUE;
      }
   }

   str = message;
   color = start_color;
   style = start_style;
   for (ptr = message; *ptr != 0; ptr++)
   {
      if (strchr(format_chars, *ptr) == NULL)
	 continue;

      ch = *(ptr + 1);

      // Check for format char right before end of string
      if (ch == 0)
	 break;

      new_type = 0;
      for (i=0; i < num_format_codes; i++)
      {
	 if (code_table[i].code != ch)
	    continue;

	 switch (code_table[i].type)
	 {
	 case CODE_COLOR:
	    new_color = code_table[i].data;
	    new_type |= CODE_COLOR;
	    break;

	 case CODE_STYLE:
	    switch (code_table[i].data)
	    {
	    case STYLE_NORMAL:
	       new_style = STYLE_NORMAL;
	       break;

	    case STYLE_RESET:
	       new_style = start_style;
	       new_color = start_color;
	       new_type |= CODE_COLOR;
	       break;
	       
	    default:
	       new_style = style ^ code_table[i].data;  // Toggle style
	       break;
	    }
	       
	    new_type |= CODE_STYLE;
	    break;

	 default:
	    debug(("DisplayServerMessage got unknown code type %d\n", code_table[i].type));
	    break;
	 }
      }
      
      if (new_type != 0)
      {
	 *ptr = 0;
	 EditBoxAddText(str, color, style);
	 if (new_type & CODE_COLOR)
	    color = new_color;
	 if (new_type & CODE_STYLE)
	    style = new_style;
	 ptr++;
	 str = ptr + 1;   // Skip code
      }
   }

   // Add anything remaining
   if (*str != 0)
      EditBoxAddText(str, color, style);

   if (bFree)
      SafeFree(message);

   SafeFree(p);
}
