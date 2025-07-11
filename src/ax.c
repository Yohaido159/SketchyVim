#include "ax.h"
#include "buffer.h"

void ax_begin(struct ax *ax)
{
  buffer_begin(&ax->buffer);
  ax->system_element = NULL;
  ax->selected_element = NULL;
  ax->role = 0;

  const void *keys[] = {kAXTrustedCheckOptionPrompt};
  const void *values[] = {kCFBooleanTrue};

  CFDictionaryRef options;
  options = CFDictionaryCreate(kCFAllocatorDefault,
                               keys, values, sizeof(keys) / sizeof(*keys),
                               &kCFCopyStringDictionaryKeyCallBacks,
                               &kCFTypeDictionaryValueCallBacks);

  ax->is_privileged = AXIsProcessTrustedWithOptions(options);
  CFRelease(options);

  if (ax->is_privileged)
    ax->system_element = AXUIElementCreateSystemWide();
  else
  {
    printf("Accessibility not granted. Exit.");
    exit(1);
  }

  assert(ax->system_element != NULL);
}

static inline bool ax_get_text(struct ax *ax)
{
  CFTypeRef text_ref = NULL;
  AXError error = AXUIElementCopyAttributeValue(ax->selected_element,
                                                kAXValueAttribute,
                                                &text_ref);
  if (error == kAXErrorSuccess)
  {
    char *raw = cfstring_get_cstring(text_ref);
    if (!raw)
    {
      CFRelease(text_ref);
      return false;
    }

    if (!ax->buffer.raw || !(strcmp(ax->buffer.raw, raw) == 0))
    {
      if (ax->buffer.raw)
        free(ax->buffer.raw);
      ax->buffer.raw = raw;
      buffer_revsync_text(&ax->buffer);
    }
    else
      free(raw);
  }

  if (text_ref)
    CFRelease(text_ref);
  return error == kAXErrorSuccess;
}

static inline bool ax_get_cursor(struct ax *ax)
{
  CFTypeRef text_range_ref = NULL;
  CFRange text_range = CFRangeMake(0, 0);
  AXError error = AXUIElementCopyAttributeValue(ax->selected_element,
                                                kAXSelectedTextRangeAttribute,
                                                &text_range_ref);

  if (error == kAXErrorSuccess)
  {
    AXValueGetValue(text_range_ref, kAXValueCFRangeType, &text_range);

    if (ax->buffer.cursor.position != text_range.location ||
        ax->buffer.cursor.selection != text_range.length)
    {
      ax->buffer.cursor.position = text_range.location;
      ax->buffer.cursor.selection = text_range.length;
      buffer_revsync_cursor(&ax->buffer);
    }
  }

  if (text_range_ref)
    CFRelease(text_range_ref);
  return error == kAXErrorSuccess;
}

static inline bool ax_set_text(struct ax *ax)
{
  if (!ax->is_supported || !ax->buffer.raw)
    return false;
  if (!ax->buffer.did_change)
    return true;

  CFStringRef text_ref = CFStringCreateWithCString(NULL,
                                                   ax->buffer.raw,
                                                   kCFStringEncodingUTF8);

  AXError error = AXUIElementSetAttributeValue(ax->selected_element,
                                               kAXValueAttribute,
                                               text_ref);

  CFRelease(text_ref);
  return error == kAXErrorSuccess;
}

static inline bool ax_set_cursor(struct ax *ax)
{
  if (!ax->is_supported)
    return false;

  CFRange text_range = CFRangeMake(ax->buffer.cursor.position,
                                   ax->buffer.cursor.selection);
  AXValueRef value = AXValueCreate(kAXValueCFRangeType, &text_range);
  // HACK: This is needed when the text has been set to give the
  // HACK: AX API some time to breathe...
  if (ax->buffer.did_change)
    usleep(15000);

  AXError error = AXUIElementSetAttributeValue(ax->selected_element,
                                               kAXSelectedTextRangeAttribute,
                                               value);

  CFRelease(value);
  return error == kAXErrorSuccess;
}

static inline bool ax_set_buffer(struct ax *ax)
{
  return ax_set_text(ax) && ax_set_cursor(ax);
}

static inline bool ax_get_selected_element(struct ax *ax)
{
  CFTypeRef selected_element = NULL;
  AXError error = AXUIElementCopyAttributeValue(ax->system_element,
                                                kAXFocusedUIElementAttribute,
                                                &selected_element);

  if (ax->selected_element && selected_element && CFEqual(ax->selected_element, selected_element))
  {
    CFRelease(selected_element);
    return true;
  }
  ax_clear(ax);

  uint32_t role = 0;
  CFTypeRef role_ref = NULL;

  if (selected_element)
  {
    AXUIElementCopyAttributeValue(selected_element,
                                  kAXRoleAttribute,
                                  &role_ref);

    if (role_ref && (CFEqual(role_ref, kAXTextFieldRole) ||
                     CFEqual(role_ref, kAXTextAreaRole) ||
                     CFEqual(role_ref, kAXComboBoxRole)))
    {
      role = ROLE_TEXT;
    }
    else if (role_ref && (CFEqual(role_ref, kAXTableRole) ||
                          CFEqual(role_ref, kAXButtonRole) ||
                          CFEqual(role_ref, kAXOutlineRole)))
    {
      role = ROLE_TABLE;
    }
    else if (role_ref && CFEqual(role_ref, kAXGroupRole))
    {
      role = ROLE_SCROLL;
    }
    else if (role_ref)
    {
      // char* role = cfstring_get_cstring(role_ref);
      // printf("Role: %s\n", role);
      // free(role);
    }

    if (role_ref)
      CFRelease(role_ref);

    if (!role)
    {
      CFRelease(selected_element);
      selected_element = NULL;
    }
  }

  ax->role = role;
  ax->selected_element = selected_element;

  return (error == kAXErrorSuccess) && role;
}

bool ax_process_selected_element(struct ax *ax)
{
  ax->is_supported = ax_get_selected_element(ax);

  bool success = true;
  if (ax->role == ROLE_TEXT && ax->buffer.cursor.mode != INSERT)
  {
    success = ax_get_text(ax) && ax_get_cursor(ax);
  }

  return ax->is_supported && success;
}

void ax_front_app_changed(struct ax *ax, pid_t pid)
{
#ifdef MANUAL_AX
  AXUIElementRef app = AXUIElementCreateApplication(pid);
  if (app)
  {
    AXUIElementSetAttributeValue(app,
                                 CFSTR("AXManualAccessibility"),
                                 kCFBooleanTrue);
    CFRelease(app);
  }
#endif
}

CGEventRef ax_process_event(struct ax *ax, CGEventRef event)
{
  if (!ax_process_selected_element(ax))
    return event;

  UniCharCount count;
  UniChar character;
  CGEventKeyboardGetUnicodeString(event, 1, &count, &character);
  CGEventFlags flags = CGEventGetFlags(event);

  // int keycode = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
  // printf("%lc 0x%x %d\n", character, character, keycode);

  // Command
  if (flags & FLAG_COMMAND)
    return event;

  // Shift Enter
  if (character == ENTER && (flags & FLAG_SHIFT))
    return event;

  // Shift Escape
  if (character == ESCAPE && (flags & FLAG_SHIFT))
    return event;

  if (ax->role == ROLE_TEXT)
  {
    // Escape in normal mode
    if (character == ESCAPE && ax->buffer.cursor.mode & NORMAL)
      return event;

    // Enter in normal mode
    if (character == ENTER && ax->buffer.cursor.mode & NORMAL)
      return event;

    bool was_insert = ax->buffer.cursor.mode & INSERT || !ax->buffer.cursor.mode;
    buffer_input(&ax->buffer, character, count);

    // Insert mode is passed and only synced later
    if (was_insert && ax->buffer.cursor.mode & INSERT)
      return event;
    else if (was_insert)
    {
      if (!ax_get_text(ax) || !ax_get_cursor(ax))
        return event;
    }

    ax_set_buffer(ax);

    return NULL;
  }

#ifdef GUI_MOVES
  // NOTE: Gui movement is currently hardcoded for my movement keys jklö and
  // NOTE: only available when compiling with the -DGUI_MOVES flag
  if (ax->role == ROLE_TABLE || ax->role == ROLE_SCROLL)
  {
    switch (character)
    {
    case K:
    {
      CGEventSetIntegerValueField(event, kCGKeyboardEventAutorepeat, false);
      CGEventSetIntegerValueField(event, kCGKeyboardEventKeycode, 125);
    }
    break;
    case L:
    {
      CGEventSetIntegerValueField(event, kCGKeyboardEventAutorepeat, false);
      CGEventSetIntegerValueField(event, kCGKeyboardEventKeycode, 126);
    }
    break;
    case J:
    {
      CGEventSetIntegerValueField(event, kCGKeyboardEventAutorepeat, false);
      CGEventSetIntegerValueField(event, kCGKeyboardEventKeycode, 123);
    }
    break;
    case OE:
    {
      CGEventSetIntegerValueField(event, kCGKeyboardEventAutorepeat, false);
      CGEventSetIntegerValueField(event, kCGKeyboardEventKeycode, 124);
    }
    break;
    }
  }
#endif // GUI_MOVES

  return event;
}

void ax_clear(struct ax *ax)
{
  buffer_clear(&ax->buffer);
  if (ax->selected_element && ax->role == ROLE_TEXT)
  {
    buffer_call_script(&ax->buffer, false);
  }

  if (ax->selected_element)
    CFRelease(ax->selected_element);
  ax->role = 0;
  ax->selected_element = NULL;
  ax->is_supported = false;
}

void ax_end(struct ax *ax)
{
  ax_clear(ax);
  if (ax->system_element)
  {
    CFRelease(ax->system_element);
    ax->system_element = NULL;
  }
}
