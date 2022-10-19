/***************************************************************************************
 * Copyright (c) 2014-2022 Zihao Yu, Nanjing University
 *
 * NEMU is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 *
 * See the Mulan PSL v2 for more details.
 ***************************************************************************************/

#include <isa.h>
#include <memory/paddr.h>
#include "ftracer.h"
#include <elf.h>
#include <common.h>
// #include "ftracer.h"
void init_rand();
void init_log(const char *log_file);
void init_mem();
void init_difftest(char *ref_so_file, long img_size, int port);
void init_device();
void init_sdb();
void init_disasm(const char *triple);

static void welcome()
{
  Log("Trace: %s", MUXDEF(CONFIG_TRACE, ANSI_FMT("ON", ANSI_FG_GREEN), ANSI_FMT("OFF", ANSI_FG_RED)));
  IFDEF(CONFIG_TRACE, Log("If trace is enabled, a log file will be generated "
                          "to record the trace. This may lead to a large log file. "
                          "If it is not necessary, you can disable it in menuconfig"));
  Log("Build time: %s, %s", __TIME__, __DATE__);
  printf("Welcome to %s-NEMU!\n", ANSI_FMT(str(__GUEST_ISA__), ANSI_FG_YELLOW ANSI_BG_RED));
  printf("For help, type \"help\"\n");
}

#ifndef CONFIG_TARGET_AM
#include <getopt.h>

void sdb_set_batch_mode();

static char *log_file = NULL;
static char *diff_so_file = NULL;
static char *img_file = NULL;
static char *elf_file = NULL;
static int difftest_port = 1234;

static long load_img()
{
  if (img_file == NULL)
  {
    Log("No image is given. Use the default build-in image.");
    return 4096; // built-in image size
  }

  FILE *fp = fopen(img_file, "rb");
  Assert(fp, "Can not open '%s'", img_file);

  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);

  Log("The image is %s, size = %ld", img_file, size);

  fseek(fp, 0, SEEK_SET);
  int ret = fread(guest_to_host(RESET_VECTOR), size, 1, fp);
  assert(ret == 1);

  fclose(fp);
  return size;
}

static void load_elf()
{
  if (elf_file == NULL)
  {
    Log("No elf is given. Use the default build-in image.");
    // return 4096; // built-in image size
    return;
  }

  FILE *fp = fopen(elf_file, "rb");
  Assert(fp, "Can not open '%s'", elf_file);
  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  void *elf_buf = malloc(size);
  Log("The elf is %s, size = %ld", elf_file, size);
  fseek(fp, 0, SEEK_SET);
  int succ = fread(elf_buf, size, 1, fp);
  if (succ != 1)
  {
    panic("read elf failed!");
  }
  fclose(fp);

  Elf32_Ehdr *elf_ehdr = elf_buf;

  for (int i = 0; i < elf_ehdr->e_phnum; ++i)
  {
    int phdr_off = i * elf_ehdr->e_phentsize + elf_ehdr->e_phoff;
    Elf32_Phdr *elf_phdr = elf_buf + phdr_off;
    if (elf_phdr->p_type != PT_LOAD)
      continue;
    void *segment_ptr = guest_to_host(elf_phdr->p_vaddr);
    memcpy(segment_ptr, elf_buf + elf_phdr->p_offset, elf_phdr->p_filesz);
    memset(segment_ptr + elf_phdr->p_filesz, 0, elf_phdr->p_memsz - elf_phdr->p_filesz);
  }

  Elf32_Shdr *symtab_shdr = NULL;
  Elf32_Shdr *shstrtab_shdr = (elf_ehdr->e_shstrndx * elf_ehdr->e_shentsize + elf_ehdr->e_shoff) + elf_buf;
  Elf32_Shdr *strtab_shdr = NULL;
  char *shstrtab_ptr = elf_buf + shstrtab_shdr->sh_offset;
  for (int i = 0; i < elf_ehdr->e_shnum; ++i)
  {
    int shdr_off = i * elf_ehdr->e_shentsize + elf_ehdr->e_shoff;
    Elf32_Shdr *elf_shdr = elf_buf + shdr_off;
    if (elf_shdr->sh_type == SHT_SYMTAB)
      symtab_shdr = elf_shdr;
    else if (elf_shdr->sh_type == SHT_STRTAB)
    {
      if (strcmp(shstrtab_ptr + elf_shdr->sh_name, ".strtab") == 0)
      {
        strtab_shdr = elf_shdr;
      }
    }
  }
  if (symtab_shdr != NULL)
  {
    char *strtab_ptr = elf_buf + strtab_shdr->sh_offset;
    for (int i = 0; i < symtab_shdr->sh_size; i += symtab_shdr->sh_entsize)
    {
      //* i work as offset here
      Elf32_Sym *elf_sym = elf_buf + symtab_shdr->sh_offset + i;
      // ! some symbol is SECTION type, so name not stored in .strtab
      if (ELF32_ST_TYPE(elf_sym->st_info) == STT_FUNC)
      {
        printf("func-symbol: %s \t size:%d \n ", strtab_ptr + elf_sym->st_name,elf_sym->st_size);
        functab_push(strtab_ptr + elf_sym->st_name, elf_sym->st_value, elf_sym->st_size);
      }
    }
    functab_print();
  }
  else
  {
    Log("No SYMTAB found");
  }
  // #endif
  free(elf_buf);
  // one malloc one free
  return;
}

static int parse_args(int argc, char *argv[])
{
  const struct option table[] = {
      {"batch", no_argument, NULL, 'b'},
      {"log", required_argument, NULL, 'l'},
      {"diff", required_argument, NULL, 'd'},
      {"port", required_argument, NULL, 'p'},
      {"help", no_argument, NULL, 'h'},
      {"elf", required_argument, NULL, 'e'},
      {0, 0, NULL, 0},
  };
  int o;
  while ((o = getopt_long(argc, argv, "-bhl:e:d:p:", table, NULL)) != -1)
  {
    switch (o)
    {
    case 'b':
      sdb_set_batch_mode();
      break;
    case 'p':
      sscanf(optarg, "%d", &difftest_port);
      break;
    case 'l':
      log_file = optarg;
      break;
    case 'd':
      diff_so_file = optarg;
      break;
    case 'e':
      elf_file = optarg;
      break;
    case 1:
      img_file = optarg;
      return 0;
    default:
      printf("\nUsage: %s [OPTION...] IMAGE [args]\n\n", argv[0]);
      printf("\t-b,--batch              run with batch mode\n");
      printf("\t-l,--log=FILE           output log to FILE\n");
      printf("\t-d,--diff=REF_SO        run DiffTest with reference REF_SO\n");
      printf("\t-p,--port=PORT          run DiffTest with port PORT\n");
      printf("\n");
      exit(0);
    }
  }
  return 0;
}

void init_monitor(int argc, char *argv[])
{
  /* Perform some global initialization. */

  /* Parse arguments. */
  parse_args(argc, argv);

  /* Set random seed. */
  init_rand();

  /* Open the log file. */
  init_log(log_file);

  /* Initialize memory. */
  init_mem();

  /* Initialize devices. */
  IFDEF(CONFIG_DEVICE, init_device());

  /* Perform ISA dependent initialization. */
  init_isa();

  /* Load the image to memory. This will overwrite the built-in image. */
  long img_size = load_img();
  if (elf_file)
  {
    load_elf();
    // img_size = load_elf();
  }
  /* Initialize differential testing. */
  init_difftest(diff_so_file, img_size, difftest_port);

  /* Initialize the simple debugger. */
  init_sdb();

  IFDEF(CONFIG_ITRACE, init_disasm(
                           MUXDEF(CONFIG_ISA_x86, "i686",
                                  MUXDEF(CONFIG_ISA_mips32, "mipsel",
                                         MUXDEF(CONFIG_ISA_riscv32, "riscv32",
                                                MUXDEF(CONFIG_ISA_riscv64, "riscv64", "bad")))) "-pc-linux-gnu"));

  /* Display welcome message. */
  welcome();
}
#else // CONFIG_TARGET_AM
static long load_img()
{
  extern char bin_start, bin_end;
  size_t size = &bin_end - &bin_start;
  Log("img size = %ld", size);
  memcpy(guest_to_host(RESET_VECTOR), &bin_start, size);
  return size;
}

void am_init_monitor()
{
  init_rand();
  init_mem();
  init_isa();
  load_img();
  IFDEF(CONFIG_DEVICE, init_device());
  welcome();
}
#endif
