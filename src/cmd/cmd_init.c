/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"
#include "casi/ctx.h"
#include "casi/shared_config.h"
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static int valid_url(const char *s) { const char *p; if (!s || !*s) return casi_error_set(CASI_EINVAL,"remote URL is empty"); if (strstr(s,"](")) return casi_error_set(CASI_EINVAL,"this looks like a pasted Markdown link; run `casi init <url>` with the plain URL only"); for(p=s;*p;p++) if (*p==' '||*p=='\t'||*p=='\n'||*p=='\r') return casi_error_set(CASI_EINVAL,"remote URL contains spaces; paste the URL alone"); return CASI_OK; }
static int pass(const char *prompt,casi_buf *out) { char b[4096]; size_t n; struct termios saved, hidden; int tty=isatty(fileno(stdin)); fputs(prompt,stderr); fflush(stderr); if(tty && tcgetattr(fileno(stdin),&saved)==0) { hidden=saved; hidden.c_lflag&=(tcflag_t)~ECHO; tcsetattr(fileno(stdin),TCSAFLUSH,&hidden); } if(!fgets(b,sizeof(b),stdin)) { if(tty) tcsetattr(fileno(stdin),TCSAFLUSH,&saved); return casi_error_set(CASI_EIO,"cannot read recovery passphrase"); } if(tty) { tcsetattr(fileno(stdin),TCSAFLUSH,&saved); fputc('\n',stderr); } n=strcspn(b,"\r\n"); if(!n) return casi_error_set(CASI_EINVAL,"recovery passphrase cannot be empty"); return casi_buf_put(out,b,n); }
static int machine(casi_config *c,casi_repo *repo,casi_buf *out) { casi_buf old=CASI_BUF_INIT,base=CASI_BUF_INIT; casi_strvec all=CASI_STRVEC_INIT; size_t i; int n=1,rc=casi_config_get_string(c,"core.machine",&old); if(rc==CASI_OK) rc=casi_buf_put(out,old.ptr,old.len); else { casi_error_clear(); rc=casi_fs_hostname(&base); if(rc==CASI_OK) rc=casi_buf_put(out,base.ptr,base.len); } if(rc!=CASI_OK) goto done; if(old.len==0 && (rc=casi_repo_list_machines(repo,&all))==CASI_OK) for(;;) { bool used=false; for(i=0;i<all.len;i++) if(!strcmp(all.items[i],casi_buf_cstr(out))) used=true; if(!used) break; casi_buf_clear(out); if((rc=casi_buf_printf(out,"%s-%d",casi_buf_cstr(&base),++n))!=CASI_OK) break; } if(rc==CASI_OK) rc=casi_repo_validate_machine_name(casi_buf_cstr(out)); done: casi_buf_dispose(&old); casi_buf_dispose(&base); casi_strvec_dispose(&all); return rc; }

int casi_cmd_init(int argc,char **argv)
{
    casi_config *cfg=NULL; casi_repo *repo=NULL; casi_shared_config shared={0}; casi_crypto crypto={0};
    casi_buf name=CASI_BUF_INIT, p=CASI_BUF_INIT, confirm=CASI_BUF_INIT, envelope=CASI_BUF_INIT, key=CASI_BUF_INIT;
    bool envelope_present=false, config_present=false, encrypted=false; int rc;
    if(argc!=1) return casi_error_set(CASI_EINVAL,"usage: %s",casi_command_lookup("init")->usage);
    if((rc=valid_url(argv[0]))!=CASI_OK || (rc=casi_config_open(&cfg))!=CASI_OK ||
       (rc=casi_repo_init(&repo))!=CASI_OK || (rc=casi_repo_set_remote(repo,argv[0]))!=CASI_OK ||
       (rc=casi_repo_fetch(repo))!=CASI_OK || (rc=machine(cfg,repo,&name))!=CASI_OK) goto done;
    if((rc=casi_shared_config_load_recovery_envelope(repo,&envelope,&envelope_present))!=CASI_OK) goto done;
    if(envelope_present) {
        if((rc=pass("Recovery passphrase: ",&p))!=CASI_OK ||
           (rc=casi_crypto_recovery_unwrap(&envelope,casi_buf_cstr(&p),(unsigned char[CASI_CRYPTO_KEY_BYTES]){0}))!=CASI_OK) goto done;
        /* Re-run into key after authentication succeeds, then persist atomically. */
        casi_buf_clear(&key); if((rc=casi_buf_grow(&key,CASI_CRYPTO_KEY_BYTES))!=CASI_OK ||
           (rc=casi_crypto_recovery_unwrap(&envelope,casi_buf_cstr(&p),(unsigned char *)key.ptr))!=CASI_OK) goto done;
        key.len=CASI_CRYPTO_KEY_BYTES;
        if((rc=casi_fs_write_file_atomic_private(casi_paths_key_file(),key.ptr,key.len))!=CASI_OK ||
           (rc=casi_crypto_load_key_file(casi_paths_key_file(),&crypto))!=CASI_OK ||
           (rc=casi_shared_config_load_remote(repo,&crypto,&shared,&config_present))!=CASI_OK) goto done;
        encrypted=true;
    } else if((rc=casi_shared_config_load_remote(repo,NULL,&shared,&config_present))!=CASI_OK) {
        casi_error_clear();
        if(!casi_fs_exists(casi_paths_key_file())) { rc=casi_error_set(CASI_EINVAL,"encrypted remote has no recovery envelope; copy its existing keyfile before init"); goto done; }
        if((rc=casi_crypto_load_key_file(casi_paths_key_file(),&crypto))!=CASI_OK ||
           (rc=casi_shared_config_load_remote(repo,&crypto,&shared,&config_present))!=CASI_OK) goto done;
        encrypted=true;
    }
    if(!config_present && !encrypted) {
        if((rc=pass("Create recovery passphrase: ",&p))!=CASI_OK || (rc=pass("Confirm recovery passphrase: ",&confirm))!=CASI_OK) goto done;
        if(strcmp(casi_buf_cstr(&p),casi_buf_cstr(&confirm))) { rc=casi_error_set(CASI_EINVAL,"recovery passphrases do not match"); goto done; }
        if((rc=casi_crypto_generate_key_file(casi_paths_key_file()))!=CASI_OK ||
           (rc=casi_fs_read_file_private(casi_paths_key_file(),&key))!=CASI_OK ||
           (rc=casi_crypto_recovery_wrap((unsigned char *)key.ptr,casi_buf_cstr(&p),&envelope))!=CASI_OK ||
           (rc=casi_crypto_load_key_file(casi_paths_key_file(),&crypto))!=CASI_OK ||
           (rc=casi_shared_config_commit_push(repo,&shared,&crypto,casi_buf_cstr(&name),&envelope))!=CASI_OK) goto done;
        encrypted=true;
    }
    if((rc=casi_config_set_string(cfg,"core.machine",casi_buf_cstr(&name)))!=CASI_OK ||
       (rc=casi_config_set_string(cfg,"core.provider",casi_provider_default()->name))!=CASI_OK) goto done;
    if(encrypted && ((rc=casi_config_set_string(cfg,"crypto.mode",CASI_CRYPTO_MODE_CONVERGENT))!=CASI_OK ||
       (rc=casi_config_set_string(cfg,"crypto.keyfile",casi_paths_key_file()))!=CASI_OK)) goto done;
    casi_info("casi store ready for machine \"%s\"",casi_buf_cstr(&name)); rc=CASI_OK;
done: casi_shared_config_dispose(&shared); casi_crypto_dispose(&crypto); casi_config_free(cfg); casi_repo_free(repo); casi_buf_dispose(&name); casi_buf_dispose(&p); casi_buf_dispose(&confirm); casi_buf_dispose(&envelope); casi_buf_dispose(&key); return rc;
}
