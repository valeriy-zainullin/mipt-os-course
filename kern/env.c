/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/elf.h>

#include <kern/env.h>
#include <kern/monitor.h>
#include <kern/sched.h>
#include <kern/kdebug.h>
#include <kern/macro.h>
#include <kern/traceopt.h>

/* Currently active environment */
struct Env *curenv = NULL;

#ifdef CONFIG_KSPACE
/* All environments */
struct Env env_array[NENV];
struct Env *envs = env_array;
#else
/* All environments */
struct Env *envs = NULL;
#endif

/* Free environment list
 * (linked by Env->env_link) */
static struct Env *env_free_list;


/* NOTE: Should be at least LOGNENV */
#define ENVGENSHIFT 12

/* Converts an envid to an env pointer.
 * If checkperm is set, the specified environment must be either the
 * current environment or an immediate child of the current environment.
 *
 * RETURNS
 *     0 on success, -E_BAD_ENV on error.
 *   On success, sets *env_store to the environment.
 *   On error, sets *env_store to NULL. */
int
envid2env(envid_t envid, struct Env **env_store, bool need_check_perm) {
    struct Env *env;

    /* If envid is zero, return the current environment. */
    if (!envid) {
        *env_store = curenv;
        return 0;
    }

    /* Look up the Env structure via the index part of the envid,
     * then check the env_id field in that struct Env
     * to ensure that the envid is not stale
     * (i.e., does not refer to a _previous_ environment
     * that used the same slot in the envs[] array). */
    env = &envs[ENVX(envid)];
    if (env->env_status == ENV_FREE || env->env_id != envid) {
        *env_store = NULL;
        return -E_BAD_ENV;
    }

    /* Check that the calling environment has legitimate permission
     * to manipulate the specified environment.
     * If checkperm is set, the specified environment
     * must be either the current environment
     * or an immediate child of the current environment. */
    if (need_check_perm && env != curenv && env->env_parent_id != curenv->env_id) {
        *env_store = NULL;
        return -E_BAD_ENV;
    }

    *env_store = env;
    return 0;
}

/* Mark all environments in 'envs' as free, set their env_ids to 0,
 * and insert them into the env_free_list.
 * Make sure the environments are in the free list in the same order
 * they are in the envs array (i.e., so that the first call to
 * env_alloc() returns envs[0]).
 */
void
env_init(void) {
    /* Set up envs array */

    // LAB 3: Your code here
    for (size_t i = 0; i < NENV; ++i) {
        struct Env* env = &envs[i];
        env->env_status = ENV_FREE;
        env->env_id = 0;

        if (i < NENV - 1) {
            env->env_link = &envs[i + 1];
        } else {
            env->env_link = NULL;
        }

    }
    
    env_free_list = &envs[0];
}

/* Allocates and initializes a new environment.
 * On success, the new environment is stored in *newenv_store.
 *
 * Returns
 *     0 on success, < 0 on failure.
 * Errors
 *    -E_NO_FREE_ENV if all NENVS environments are allocated
 *    -E_NO_MEM on memory exhaustion
 */
int
env_alloc(struct Env **newenv_store, envid_t parent_id, enum EnvType type) {

    struct Env *env;
    if (!(env = env_free_list))
        return -E_NO_FREE_ENV;

    /* Generate an env_id for this environment */
    int32_t generation = (env->env_id + (1 << ENVGENSHIFT)) & ~(NENV - 1);
    /* Don't create a negative env_id */
    if (generation <= 0) generation = 1 << ENVGENSHIFT;
    env->env_id = generation | (env - envs);

    /* Set the basic status variables */
    env->env_parent_id = parent_id;
#ifdef CONFIG_KSPACE
    env->env_type = ENV_TYPE_KERNEL;
#else
    env->env_type = type;
#endif
    env->env_status = ENV_RUNNABLE;
    env->env_runs = 0;

    /* Clear out all the saved register state,
     * to prevent the register values
     * of a prior environment inhabiting this Env structure
     * from "leaking" into our new environment */
    memset(&env->env_tf, 0, sizeof(env->env_tf));

    /* Set up appropriate initial values for the segment registers.
     * GD_UD is the user data (KD - kernel data) segment selector in the GDT, and
     * GD_UT is the user text (KT - kernel text) segment selector (see inc/memlayout.h).
     * The low 2 bits of each segment register contains the
     * Requestor Privilege Level (RPL); 3 means user mode, 0 - kernel mode.  When
     * we switch privilege levels, the hardware does various
     * checks involving the RPL and the Descriptor Privilege Level
     * (DPL) stored in the descriptors themselves */

#ifdef CONFIG_KSPACE
    env->env_tf.tf_ds = GD_KD;
    env->env_tf.tf_es = GD_KD;
    env->env_tf.tf_ss = GD_KD;
    env->env_tf.tf_cs = GD_KT;

    // LAB 3: Your code here:
    static const uintptr_t STACK_AREA_TOP = 0x2000000;
    
    // Check there is space for the stack of the newly created process.
    // NENV now is 2^10 = 1024. If will be more at some point,
    // our current memory allocation strategy could be dangerous,
    // as STACK_AREA could be exausted. Check it won't be for sure.
    // Otherwise, if it will be possible, return that error has occured.
    // Or change allocation strategy, I think this is better.
    assert((NENV - 1) * 2 * PAGE_SIZE < STACK_AREA_TOP);
    
    // FIXME: stack may start colliding with the already loaded binaries.
    //   And loaded binaries in future.

    size_t env_index = (size_t) (env - envs);
    uintptr_t stack_area_offset = 2 * PAGE_SIZE * env_index;
    env->env_tf.tf_rsp = STACK_AREA_TOP - stack_area_offset;

#else
    env->env_tf.tf_ds = GD_UD | 3;
    env->env_tf.tf_es = GD_UD | 3;
    env->env_tf.tf_ss = GD_UD | 3;
    env->env_tf.tf_cs = GD_UT | 3;
    env->env_tf.tf_rsp = USER_STACK_TOP;
#endif

    /* Commit the allocation */
    env_free_list = env->env_link;
    *newenv_store = env;

    if (trace_envs) cprintf("[%08x] new env %08x\n", curenv ? curenv->env_id : 0, env->env_id);
    return 0;
}

/* Pass the original ELF image to binary/size and bind all the symbols within
 * its loaded address space specified by image_start/image_end.
 * Make sure you understand why you need to check that each binding
 * must be performed within the image_start/image_end range.
 */
static int
bind_functions(struct Env *env, uint8_t *binary, size_t size, uintptr_t image_start, uintptr_t image_end) {
    // LAB 3: Your code here:

    /* NOTE: find_function from kdebug.c should be used */

    // TODO: add sensible values to image_start and image_end if possible.
    //   As the API of load_icode is already fixed. If there are no fields
    //   for image_start and image_end in the env struct, that won't be possibe.

    // User programs have symbols in their symbol table. We just resolve them.
    // For now it is based on this array.
    // I suppose, later there will be syscalls and we won't do any binding.

    // Find function entry in the user-space program ELF header's symbols.
    // The elf header was already checked for validity in load_icode.
    //   Also load_icode checks section header is fully in file.
    //   So that we won't use uninitialized memory.
    struct Elf const* elf_header = (struct Elf const*) env->binary;

    // Overflows are not possible here, as if an offset is greater than max value
    //   of size_t, then it is greater than size. size has type size_t, that's why.
    //   Otherwise, it would be possible.
    //   load_icode checks elf_header->e_shoff is less than size.
    //   Also load_icode checks section headers are fully inside of the file.
    struct Secthdr const* section_headers = (struct Secthdr const*) (binary + elf_header->e_shoff);

    struct Secthdr const* sectname_section_header = &section_headers[elf_header->e_shstrndx];
    // This may be SHN_UNDEF. The only way to check it is to see what is the type of the section.
    // If it is strtab, we don't have a way to distinguish if it is SHN_UNDEF or a valid section
    // index.
    if (sectname_section_header->sh_type != ELF_SHT_STRTAB) {
        // No section names. Then we won't be able to find the section with the names of symbols.
        // There may be symbols to bind, but there's no symbol table. Fail the binding.
        // In future we will have syscalls and we won't have to act like this anymore.
        // We'll support having no section names.
        return -1;
    }

    char const * section_names_buffer = (char const*) (binary + sectname_section_header->sh_offset);

    // .bss section is PROG_NO_BITS and shows where in memory uninitialized variables
    //   will be located. If there's no such section, we won't continue binding,
    //   because only uninitialized memory can be preinitialized by the kernel.
    uintptr_t bss_section_va_start = 0;
    uintptr_t bss_section_va_past_end = 0;
    bool found_bss = false;
    for (UINT16 section_index = 0; section_index < elf_header->e_shnum; ++section_index) {
        struct Secthdr const* section_header = &section_headers[section_index];
        // FIXME: may be not null terminated!
        // FIXME: may be out of bounds (meaning pointer may be out of bounds of the file).
        if (strcmp(section_names_buffer + section_header->sh_name, ".bss") == 0) {
            // FIXME: overflows.
            bss_section_va_start = section_header->sh_addr;
            bss_section_va_past_end = bss_section_va_start + section_header->sh_size;
            found_bss = true;
            break;
        }
    }
    if (!found_bss) {
        // Can't bind. Fail the binding, because there may be symbols to bind,
        //   but there's no .bss section.
        // In future we will have syscalls and we won't have to act like this anymore.
        // We'll support having no bss section (at least, state, there may be a corresponding segment).
        return -2;
    }

    char const * symbol_names_buffer = NULL;
    for (UINT16 section_index = 0; section_index < elf_header->e_shnum; ++section_index) {
        struct Secthdr const* section_header = &section_headers[section_index];
        // FIXME: may be not null terminated! Use strncmp? Is it a good solution? I suppose.
        // FIXME: may be out of bounds.
        if (section_header->sh_type == ELF_SHT_STRTAB && strcmp(section_names_buffer + section_header->sh_name, ".strtab") == 0) {
            // load_icode checks this is fully inside of the file.
            symbol_names_buffer = (char const*) (binary + section_header->sh_offset);
            break;
        }
    }
    if (symbol_names_buffer == NULL) {
        // No symbol name table.
        // There may be symbols to bind, but there's no symbol table. Fail the binding.
        // In future we will have syscalls and we won't have to act like this anymore.
        // We'll support having no symbol table.
        return -3;
    }

    struct Secthdr const* symtbl_section_header = NULL;
    for (UINT16 section_index = 0; section_index < elf_header->e_shnum; ++section_index) {
        struct Secthdr const* section_header = &section_headers[section_index];
        if (section_header->sh_type == ELF_SHT_SYMTAB) {
            symtbl_section_header = section_header;
            break;
        }
        // Move this into load_icode, account these there.
        // SHT_NOBITS check in load_icode.
        // sh_addralign is not respected yet. load_icode
        // load_icode
        // FIXME: respect sh_addralign. Maybe fail to load executables that have
        // sections with virtual addresses not aligned on sh_addralign?
    }

    if (symtbl_section_header == NULL) {
        // No symbols to bind. No section of type symtab found.
        //   Then binding is successful, nothing to do.
        return 0;
    }

    struct Elf64_Sym const * symbol_table_entries = (struct Elf64_Sym const *) (env->binary + symtbl_section_header->sh_offset);
    size_t num_entries = symtbl_section_header->sh_size / sizeof(struct Elf64_Sym); // Rounded down.

    // I wanted to restrict the set of exported functions.
    // It's more abstract for the program, less usages, we'll be free to change
    //   unexported functions and what they do, even delete them.
    // But tests require to export all functions. So we'll have table only for asm functions.
    /*
    const struct exported_function {
        const char* user_space_fname;
        // const char* kernel_space_fname;
        uintptr_t kernel_code_address;
    } EXPORTED_FUNCTIONS[] = {
        // For debugging purposes. Also in this task we should use dwarf debugging info if possible
        { "cprintf"  , find_function("cprintf")   },
        { "sys_yield", (uintptr_t) sys_yield      },
        { "sys_exit" , (uintptr_t) sys_exit       },
    };
    static const size_t NUM_EXPORTED_FUNCTIONS = sizeof(EXPORTED_FUNCTIONS) / sizeof(struct exported_function);
    */
    const struct exported_function {
        const char* user_space_fname;
        uintptr_t kernel_code_address;
    } ASM_EXPORTED_FUNCTIONS[] = {
        { "sys_yield", (uintptr_t) sys_yield },
        { "sys_exit" , (uintptr_t) sys_exit  }
    };
    static const size_t NUM_EXPORTED_ASM_FUNCTIONS = sizeof(ASM_EXPORTED_FUNCTIONS) / sizeof(struct exported_function);
    
    /* Playing this task as a real-life developer, in case we have to do binding like this.

       For a symbol to be bounded by the kernel, it needs to be global uninitialized volatile pointer to a function.
       It could checked like this: the symbol should point to .bss section (.bss section should exist, otherwise no
       symbols are bound), we check in dwarf debugging info that type matches and it's a global variable.

       Because it's possible the file doesn't have dwarf debugging info, having the symbol point inside .bss is enough
       for us to try to find the symbol in the kernel and modify provided symbol's value. Because it's uninitialized,
       code mustn't expect it to have a particular value anyway, in case it's not a kernel function. The kernel and OS
       are also part of implementation from the C and C++ standards in that case. All uninitialized symbols, if
       they don't correspond to kernel functions, are initialized to zero by the kernel.
    */


    /*
      Если символ не найден, по-хорошему, мы должны привязывать туда sys_exit, но это
      не будет проходить тесты.
      Записывать туда 0, просто в тесте 2 есть проверка указателя на ноль, и есть вызов
      без этого, мб тот вызов не всегда должен работать, иногда крашить, а мб там всегда
      функция будет найдена. Тесты проходят, мб это хорошее решение, т.к. дальше будет
      сегментация, она будет останавливать процесс, если он попытается вызвать функцию,
      которая не была найдена. Да и syscallы будут уже к тому моменту.
    */

    // Won't overflow as sizeof(struct Elf64_sym) is at least two bytes.
    for (size_t entry_idx = 0; entry_idx < num_entries; ++entry_idx) {
        struct Elf64_Sym const * symbol_table_entry = &symbol_table_entries[entry_idx];
        // uint8_t symbol_type = ELF64_ST_TYPE(symbol_table_entry->st_info);
        // FIXME: may be not null-terminated.
        char const* symbol_name = symbol_names_buffer + symbol_table_entry->st_name;
        // cprintf("entry_idx = %zu, symbol_type = %u, symbol_name = %s.\n", entry_idx, (unsigned) symbol_type, symbol_name);

        // We don't know if it is address or not, because the symbol table entry type (st_type)
        // is not necesserily ST_FUNC or anything, but it has to be address, otherwise no way
        // we can bind the value.
        uintptr_t symbol_va = symbol_table_entry->st_value;
        uintptr_t* symbol_value = (uintptr_t*) symbol_va; // Assuming symbol value is 8 bytes.

        // Global volatile pointers to functions have global binding and object type
        //   in elf format's symbol table. Found out by an experiment. Otherwise
        //   account for cases when it's not. Also we may make it a part of our ABI.
        if (ELF_ST_BIND(symbol_table_entry->st_info) != 1 || ELF_ST_TYPE(symbol_table_entry->st_info) != 1) {
            continue;
        }

        // Must be uninitialized (reside in .bss) in order for us to even touch
        //   it's value.
        if (symbol_va < bss_section_va_start || symbol_va >= bss_section_va_past_end) {
            continue;
        }

        cprintf("symbol_name = %s.\n", symbol_name);

        // Bounding by dwarf info we'll do. Check it's [uninitialized, if possible] volatile global pointer.
        // Global means DW_AT_external is true.

        bool was_bound = false;

        for (size_t func_index = 0; func_index < NUM_EXPORTED_ASM_FUNCTIONS; ++func_index) {
            struct exported_function const * exported_func = &ASM_EXPORTED_FUNCTIONS[func_index];

            if (strcmp(exported_func->user_space_fname, symbol_name) != 0) {
                continue;
            }

            uintptr_t address = exported_func->kernel_code_address;
            // 0 means the function was not found. Checked in debug build.
            //   The set of function is constant, so if the code was unable to find a function,
            //   we'll catch the bug in the debug build anyway. Just need to retest on it.
            assert(address != 0);

            cprintf("Binding %s@%p to %p.\n", exported_func->user_space_fname, (void const*) address, symbol_value);

            // FIXME: may be not in the file (out of the file, greater than or equal to size).
            // SUGGESTION сдача: отредактировать программу так prog1, чтобы при загрузке
            //   в этом месте возникало обращение по невыровненому адресу и undefined
            //   behaviour санитайзер зарепортил. И поправить тесты.
            // Ветка lab3-suggestion будет. Объяснить, что в ней будет происходить потом в коде.
            *symbol_value = address;

            was_bound = true;
        }

        if (was_bound) {
            continue;
        }

        // Ищем функцию в отладочной информации. Ассемблерные искали отдельно, поскольку
        //   в отладочной информации их нет. Могли бы просто таблицу с экспортируемыми
        //   функциями сделать, но отладочную информацию мы всё равно должны использовать
        //   по заданию.

        uintptr_t kernel_code_address = find_function(symbol_name);
        if (kernel_code_address != 0) {
            *symbol_value = kernel_code_address;
            continue;
        } else {
            // Initialize to 0.
            *symbol_value = (uintptr_t) 0;
        }
    
        // Не найдена. В таблице не обязательно только те символы, которые
        //   нужно связать. Так что если не нашли, никаких проблем в этом нет.
        // Но программа свалится с ошибкой, если попробует вызвать по тому указателю.
        //   Не инициализированному, хотя пользователи могут сделать функцию,
        //   адресом которой инициализировали бы указатели, чтобы отлавливать ошибку.
        // Поскольку у нас пока нет защиты, всё в режиме ядра, и это нужно по заданию,
        //   давайте все глобальные переменные -- указатели на функции, будем
        //   инициализировать адресом sys_exit. Чтобы вызов по ним приводил к
        //   завершению программы. Как мы определеним, что значения нет? Тип переменной
        //   в dwarf есть, слава богу. Мы знаем, что это volatile указатель на функцию.
        //   Сходим в исполняемый файл по адресу, если там значение за пределами
        //   адресного пространства файла, записывыаем sys_exit. Да, теперь значения
        //   глобальных переменных меняются, если они всё-таки были инициализированы,
        //   но такой уж у нас будет ABI. Это только глобальные volatile указатели на
        //   функции, у них теперь специальное значение.

    }

    // No symbols to bind. No section of type symtab found.
    //   Then binding is successful, nothing to do.
    return 0;
}

/* Set up the initial program binary, stack, and processor flags
 * for a user process.
 * This function is ONLY called during kernel initialization,
 * before running the first environment.
 *
 * This function loads all loadable segments from the ELF binary image
 * into the environment's user memory, starting at the appropriate
 * virtual addresses indicated in the ELF program header.
 * At the same time it clears to zero any portions of these segments
 * that are marked in the program header as being mapped
 * but not actually present in the ELF file - i.e., the program's bss section.
 *
 * All this is very similar to what our boot loader does, except the boot
 * loader also needs to read the code from disk.  Take a look at
 * LoaderPkg/Loader/Bootloader.c to get ideas.
 *
 * Finally, this function maps one page for the program's initial stack.
 *
 * load_icode returns -E_INVALID_EXE if it encounters problems.
 *  - How might load_icode fail?  What might be wrong with the given input?
 *
 * Hints:
 *   Load each program segment into memory
 *   at the address specified in the ELF section header.
 *   You should only load segments with ph->p_type == ELF_PROG_LOAD.
 *   Each segment's address can be found in ph->p_va
 *   and its size in memory can be found in ph->p_memsz.
 *   The ph->p_filesz bytes from the ELF binary, starting at
 *   'binary + ph->p_offset', should be copied to address
 *   ph->p_va.  Any remaining memory bytes should be cleared to zero.
 *   (The ELF header should have ph->p_filesz <= ph->p_memsz.)
 *
 *   All page protection bits should be user read/write for now.
 *   ELF segments are not necessarily page-aligned, but you can
 *   assume for this function that no two segments will touch
 *   the same page.
 *
 *   You must also do something with the program's entry point,
 *   to make sure that the environment starts executing there.
 *   What?  (See env_run() and env_pop_tf() below.) */
static int
load_icode(struct Env *env, uint8_t *binary, size_t size/*, size_t* addr_space_size to support image_start, image_end arguments in bind_functions */) {
    // LAB 3: Your code here

    /* 1. Load each program segment into memory
            at the address specified in the ELF section header.
          We should only load segments with ph->p_type == ELF_PROG_LOAD.
          Each segment's address can be found in ph->p_va
            and its size in memory can be found in ph->p_memsz.
          The ph->p_filesz bytes from the ELF binary, starting at
            'binary + ph->p_offset', should be copied to address
            ph->p_va.  Any remaining memory bytes should be cleared to zero.
            (The ELF header should have ph->p_filesz <= ph->p_memsz.)
    */
    
    // FIXME: check the elf header specifies the architecture the OS running on.

    // Check the elf header fits into the file.
    //   We'd access the field of the header, we should know
    //   we won't overflow the buffer and that way won't use
    //   initialized memory.
    if (sizeof(struct Elf) > size) {
        return -E_INVALID_EXE;
    }

    struct Elf const* elf_header = (struct Elf const*) binary;
    
    // Do the same thing bootloader does. :)
    
    if (elf_header->e_magic != ELF_MAGIC) {
        // Invalid magic bytes.
        return -E_INVALID_EXE;
    }

    if (elf_header->e_type != ET_EXEC) {
        // Only executable elf files are supported.
        return -E_INVALID_EXE;
    }

    // Specifies file is 64-bit executable.
    static const UINT8 ELF_CLASS64 = 2;
    if (elf_header->e_elf[0] != ELF_CLASS64) {
        // The operating system is 64-bit.
        //   No 32-bit executable support (x86-32 compatibility mode).
        // File is not 64-bit program.
        return -E_INVALID_EXE;
    }

    // Check section header size.
    // https://refspecs.linuxfoundation.org/elf/gabi4+/ch4.eheader.html,
    // there's "e_shentsize" description that helped me to understand the meaning of the field :)
    // It's the size of one section header, one entry in section header table.
    // If it's different, it could be unsupported elf format variation (I don't know if there are different in that regard elf variations).
    if (elf_header->e_shentsize != sizeof(struct Secthdr)) {
        return -E_INVALID_EXE;
    }

    // Check section name table's section index is in range of section indices.
    // It can be SHN_UNDEF if sections aren't named.
    // All of the cases where sections aren't named or this is equal to SHN_XINDEX
    // are not supported for now.
    // FIXME: support these cases.
    if (elf_header->e_shstrndx == ELF_SHN_UNDEF) {
        // FIXME: support this case.
        return -E_INVALID_EXE;
    }
    static const UINT16 ELF_SHN_XINDEX = 0xffff; // Loader pkg doesn't provide this value.
    if (elf_header->e_shstrndx == ELF_SHN_XINDEX) {
        // FIXME: support this case.
        return -E_INVALID_EXE;
    }
    if (elf_header->e_shstrndx >= elf_header->e_shnum) {
        // Section name table's section index is out of range of section indices.
        return -E_INVALID_EXE;
    }

    // Check program header entry size is supported.
    if (elf_header->e_phentsize != sizeof(struct Proghdr)) {
        return -E_INVALID_EXE;
    }

    // The loaded executable must have segment (program) headers.
    // Segments are different to sections in that they are needed for running
    //   the program and sections are needed for linking and relocation.
    //   See https://en.wikipedia.org/wiki/Executable_and_Linkable_Format#File_layout,
    //   second paragraph.
    // This member being zero means it does not.
    // According to https://refspecs.linuxfoundation.org/elf/gabi4+/ch4.eheader.html
    if (elf_header->e_phoff == 0) {
        return -E_INVALID_EXE;
    }

    // Check section header offset is in range.
    //  To not get uninitialized memory usage problems in the kernel.
    //  If it is zero, it should still work.
    if (elf_header->e_shoff >= size) {
        return -E_INVALID_EXE;
    }

    // Check program header offset is in range.
    //  To not get uninitialized memory usage problems in the kernel.
    if (elf_header->e_phoff >= size) {
        return -E_INVALID_EXE;
    }

    // Overflows are not possible here, as if an offset is greater than max value
    //   of size_t, then it is greater than size. size has type size_t, that's why.
    //   Otherwise, it would be possible.
    struct Proghdr const* program_headers = (struct Proghdr const*) (binary + elf_header->e_phoff);
    struct Secthdr const* section_headers = (struct Secthdr const*) (binary + elf_header->e_shoff);

    // FIXME: check none of segments and sections overlap in virtual memory.

    // FIXME: check for overflows in the size computations below.
    // elf_header->e_shnum and elf_header->e_phnum are UINT16, elf_header->e_shoff
    //   and elf_header->e_phoff are UINT64. We won't get automatic guarantees
    //   the values won't overflow during calculations. We have to check that.
    //   That's a FIXME for now.

    // Check last section of the section table is still inside of the file.
    //  This also checks that all sections are inside of the file.
    // Number of bytes before the table (0-index of the first byte of the table)
    //   + number of bytes the table takes should be the byte after the table
    //   and not further than the byte after the file, which has 0-index of size.
    // It also works if there are zero section headers.
    if (elf_header->e_shoff + sizeof(struct Secthdr) * elf_header->e_shnum > size) {
        return -E_INVALID_EXE;
    }
    // The same for the program header table.
    if (elf_header->e_phoff + sizeof(struct Proghdr) * elf_header->e_phnum > size) {
        return -E_INVALID_EXE;
    }
    
    // UINT64 section_name_table_off = sections[elf_header->e_shstrndx].sh_offset;

    // Maybe we do need sections for debug information. Don't load it for now.
    // Implement in case needed. Look at bootloader.
    /*
    for (UINT16 section_index = 0; section_index < elf_header->e_shnum; ++section_index) {
    }
    */

    // FIXME: zero out the memory of the process, so that the kernel won't
    //   disclose it's own information or information of another previously
    //   running process.
    // I suppose it could be done the same way it's done in the bootloader
    // as long as we haven't implemented allocating a dedicated address
    // space for a process. At that point we'll have to zero out individual
    // pages allocated for address space.
    // memset((VOID *)(UINTN) MinAddress, KernelSize);
    // ALSO, it a part of the task. So we'll do it, but later.

    for (UINT16 segment_index = 0; segment_index < elf_header->e_phnum; ++segment_index) {
        struct Proghdr const* program_header = &program_headers[segment_index];
        if (program_header->p_type == PT_LOAD && program_header->p_filesz > 0) {
            // Check the segment is fully in file.
            //   To not use uninitialized memory.
            // FIXME: check for overflows.
            if (program_header->p_offset + program_header->p_filesz > size) {
                return -E_INVALID_EXE;
            }
            memcpy((void*) (size_t) program_header->p_pa, (void const*) ((char const*) binary + program_header->p_offset), program_header->p_filesz);
        }
    }

    // Also, we have to load sections with sh_addr != 0.
    // https://refspecs.linuxfoundation.org/elf/gabi4+/ch4.sheader.html
    for (UINT16 section_index = 0; section_index < elf_header->e_shnum; ++section_index) {
        struct Secthdr const* section_header = &section_headers[section_index];
        // This section is not present in the file anyways.
        //   I think it's not intended to be created in virtual memory.
        static const UINT32 ELF_SHT_NOBITS = 0x8;
        if (section_header->sh_type == ELF_SHT_NOBITS) {
            continue;
        }
        if (section_header->sh_addr != 0) {
            // Check the section is fully in file.
            //   To not use uninitialized memory.
            // FIXME: check for overflows.
            if (section_header->sh_offset + section_header->sh_size > size) {
                return -E_INVALID_EXE;
            }
            memcpy((void*) (size_t) section_header->sh_addr, (void const*) ((char const*) binary + section_header->sh_offset), section_header->sh_size);
        }
    }


    // Trap frame is already zeroed by the alloc function.
    env->binary = binary;

 /*   
 *   All page protection bits should be user read/write for now.
 *   ELF segments are not necessarily page-aligned, but you can
 *   assume for this function that no two segments will touch
 *   the same page.
 *
 *   You must also do something with the program's entry point,
 *   to make sure that the environment starts executing there.
 *   What?  (See env_run() and env_pop_tf() below.) */
    // Valid executable must have an entry point.
    // FIXME: also check it's in a segment with the right permissions. 
    if (elf_header->e_entry == 0) {
        return -E_INVALID_EXE;
    }
    env->env_tf.tf_rip = elf_header->e_entry;

    return 0;
}


int mon_backtrace(int argc, char **argv, struct Trapframe *tf);

/* Allocates a new env with env_alloc, loads the named elf
 * binary into it with load_icode, and sets its env_type.
 * This function is ONLY called during kernel initialization,
 * before running the first user-mode environment.
 * The new env's parent ID is set to 0.
 */
void
env_create(uint8_t *binary, size_t size, enum EnvType type) {
    // LAB 3: Your code here

    mon_backtrace(0, 0, 0);

    struct Env* env = NULL;
    if (env_alloc(&env, 0, type) < 0) {
        panic("failed to allocate a process during kernel initilization.");
    }

    if (load_icode(env, binary, size) < 0) {
        panic("failed to load a kernel process' image during kernel initialization.");
    }

    // Image start is the address where the elf header would start in memory,
    //   address the image was loaded into. But for us it's zero, as we
    // assume all programs are loaded at 0 address and then their segment
    // vas are equal to their rva's from program headers.
    if (bind_functions(env, binary, size, 0, ~((size_t) 0)) < 0) {
        panic("failed to bind functions for a kernel process during kernel initialization.");
    }
}


/* Frees env and all memory it uses */
void
env_free(struct Env *env) {

    /* Note the environment's demise. */
    if (trace_envs) cprintf("[%08x] free env %08x\n", curenv ? curenv->env_id : 0, env->env_id);

    /* Return the environment to the free list */
    env->env_status = ENV_FREE;
    env->env_link = env_free_list;
    env_free_list = env;
}

/* Frees environment env
 *
 * If env was the current one, then runs a new environment
 * (and does not return to the caller)
 */
void
env_destroy(struct Env *env) {
    /* If env is currently running on other CPUs, we change its state to
     * ENV_DYING. A zombie environment will be freed the next time
     * it traps to the kernel. */

    // LAB 3: Your code here

    // We don't have multiprocessing yet. Only multitasking.
    // And the OS is not preemptive for now.
    // So we modify data structures without locking.
    // The note above could be satisfied with a locked (lock
    //   the task data structure and the scheduler) check for
    //   last scheduled cpu and current cpu. If they are not
    //   the same

    // We don't need to set status to ENV_DYING as for now
    //  we are immediately freeing the task. No multiprocessing yet.
    env_free(env);

    sched_yield();
}

#ifdef CONFIG_KSPACE
void
csys_exit(void) {
    // TODO: account sys_exit should work with env structures atomically on
    //   multiprocessor systems, as otherwise other cpus may interfere with
    //   the operations on data structures.

    if (!curenv) panic("curenv = NULL");
    env_destroy(curenv);
}

void
csys_yield(struct Trapframe *tf) {
    memcpy(&curenv->env_tf, tf, sizeof(struct Trapframe));

    sched_yield();
}
#endif

/* Restores the register values in the Trapframe with the 'ret' instruction.
 * This exits the kernel and starts executing some environment's code.
 *
 * This function does not return.
 */

_Noreturn void
env_pop_tf(struct Trapframe *tf) {
    /* Push RIP on program stack */
    tf->tf_rsp -= sizeof(uintptr_t);
    *((uintptr_t *)tf->tf_rsp) = tf->tf_rip;
    /* Push RFLAGS on program stack */
    tf->tf_rsp -= sizeof(uintptr_t);
    *((uintptr_t *)tf->tf_rsp) = tf->tf_rflags;

    asm volatile(
            "movq %0, %%rsp\n"
            "movq 0(%%rsp), %%r15\n"
            "movq 8(%%rsp), %%r14\n"
            "movq 16(%%rsp), %%r13\n"
            "movq 24(%%rsp), %%r12\n"
            "movq 32(%%rsp), %%r11\n"
            "movq 40(%%rsp), %%r10\n"
            "movq 48(%%rsp), %%r9\n"
            "movq 56(%%rsp), %%r8\n"
            "movq 64(%%rsp), %%rsi\n"
            "movq 72(%%rsp), %%rdi\n"
            "movq 80(%%rsp), %%rbp\n"
            "movq 88(%%rsp), %%rdx\n"
            "movq 96(%%rsp), %%rcx\n"
            "movq 104(%%rsp), %%rbx\n"
            "movq 112(%%rsp), %%rax\n"
            "movw 120(%%rsp), %%es\n"
            "movw 128(%%rsp), %%ds\n"
            "movq (128+48)(%%rsp), %%rsp\n"
            "popfq; ret" ::"g"(tf)
            : "memory");

    /* Mostly to placate the compiler */
    panic("Reached unrecheble\n");
}

/* Context switch from curenv to env.
 * This function does not return.
 *
 * Step 1: If this is a context switch (a new environment is running):
 *       1. Set the current environment (if any) back to
 *          ENV_RUNNABLE if it is ENV_RUNNING (think about
 *          what other states it can be in),
 *       2. Set 'curenv' to the new environment,
 *       3. Set its status to ENV_RUNNING,
 *       4. Update its 'env_runs' counter,
 * Step 2: Use env_pop_tf() to restore the environment's
 *       registers and starting execution of process.

 * Hints:
 *    If this is the first call to env_run, curenv is NULL.
 *
 *    This function loads the new environment's state from
 *    env->env_tf.  Go back through the code you wrote above
 *    and make sure you have set the relevant parts of
 *    env->env_tf to sensible values.
 */
_Noreturn void
env_run(struct Env *env) {
    assert(env);

    if (trace_envs_more) {
        const char *state[] = {"FREE", "DYING", "RUNNABLE", "RUNNING", "NOT_RUNNABLE"};
        if (curenv) cprintf("[%08X] env stopped: %s\n", curenv->env_id, state[curenv->env_status]);
        cprintf("[%08X] env started: %s\n", env->env_id, state[env->env_status]);
    }

    // LAB 3: Your code here

    if (__builtin_expect(curenv != NULL, 1)) {
        assert(curenv->env_status == ENV_RUNNING || curenv->env_status == ENV_FREE || curenv->env_status == ENV_DYING);
        if (__builtin_expect(curenv->env_status == ENV_RUNNING, 1)) {
            curenv->env_status = ENV_RUNNABLE;
        } else if (curenv->env_status == ENV_FREE) {
            // The task has just exited. No need to do anything.
            (void) 0;
        } else if (curenv->env_status == ENV_DYING) {
            // Won't be reached for now, as the operating system is not preemptive.
            // If a task has reached env_destroy, it'll be surely destroyed.
            //   When the OS will have preemption, this code will be useful.
            // This code should be in the scheduler as this function does not return
            //   nor it selects the task to execute. So the scheduler should free the
            //   task and look for the next candidate for cpu time.
            // TODO: implement.
            panic("not implemented!");
            (void) 0;
        }
    }

    assert(env->env_status == ENV_RUNNABLE || env->env_status == ENV_DYING);

    curenv = env;
    curenv->env_status = ENV_RUNNING;
    curenv->env_runs += 1;
    env_pop_tf(&curenv->env_tf);

    while(1) {}
}
