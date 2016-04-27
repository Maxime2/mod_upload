APXS=apxs
APXS_CFLAGS=
APXS_LFLAGS=
APXS_LIBS=

all: mod_upload.la

install: all
	@sudo $(APXS) -i -a -n upload mod_upload.la

mod_upload.la: mod_upload.c
	@$(APXS) -c -o $@ $(APXS_CFLAGS) $(APXS_LFLAGS) $(APXS_LIBS) $< --shared

clean:
	@rm -f *~ *.la *.lo *.slo

dist-clean: clean
