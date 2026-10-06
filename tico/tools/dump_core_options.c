/* Dumps the core options the Switch build of SwanStation reads, as JSON on
 * stdout: key, label, category, default and choices. Built and run on the
 * host by tico_module.py with the aarch64 definitions, so the module never
 * drifts from what the core accepts. The core ships no translations of its
 * options; tico/lang carries tico's. */
#include <stdbool.h>
#include <stdio.h>

#include <libretro.h>

/* The header's helpers talk to the frontend; the dump only reads its tables. */
static bool g_retro_environment_callback(unsigned cmd, void *data)
{
   (void)cmd;
   (void)data;
   return false;
}

#include "libretro_core_options.h"

static void put_string(const char *s)
{
   putchar('"');
   for (; s && *s; s++)
   {
      if (*s == '"' || *s == '\\')
         putchar('\\');
      if (*s == '\n')
         fputs("\\n", stdout);
      else
         putchar(*s);
   }
   putchar('"');
}

int main(void)
{
   const struct retro_core_option_v2_definition *def;
   const struct retro_core_option_v2_category *cat;
   printf("{\n  \"categories\": [");
   for (cat = option_cats_us; cat && cat->key; cat++)
   {
      printf("%s[", cat == option_cats_us ? "" : ", ");
      put_string(cat->key);
      printf(", ");
      put_string(cat->desc);
      printf("]");
   }
   printf("],\n  \"options\": [\n");
   for (def = option_defs_us; def && def->key; def++)
   {
      const struct retro_core_option_value *value;
      printf("    {\"key\": ");
      put_string(def->key);
      printf(", \"desc\": ");
      put_string(def->desc);
      printf(", \"category\": ");
      put_string(def->category_key);
      printf(", \"info\": ");
      put_string(def->info);
      printf(", \"default\": ");
      put_string(def->default_value);
      printf(", \"values\": [");
      for (value = def->values; value->value; value++)
      {
         printf("%s[", value == def->values ? "" : ", ");
         put_string(value->value);
         printf(", ");
         put_string(value->label ? value->label : value->value);
         printf("]");
      }
      printf("]}%s\n", def[1].key ? "," : "");
   }
   printf("  ]\n}\n");
   return 0;
}
