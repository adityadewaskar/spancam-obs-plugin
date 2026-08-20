// Prints "<size>\n" then <size> lines of 0/1 for the QR encoding of argv[1].
// Build: cc -O2 -o build-aux/placeholder/emit_qr \
//            build-aux/placeholder/emit_qr.c build-aux/placeholder/qrcodegen.c
#include <stdio.h>
#include "qrcodegen.h"
int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: emit_qr <text>\n"); return 2; }
	uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
	if (!qrcodegen_encodeText(argv[1], tmp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
				  qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
		fprintf(stderr, "encode failed\n");
		return 1;
	}
	int n = qrcodegen_getSize(qr);
	printf("%d\n", n);
	for (int y = 0; y < n; y++) {
		for (int x = 0; x < n; x++)
			putchar(qrcodegen_getModule(qr, x, y) ? '1' : '0');
		putchar('\n');
	}
	return 0;
}
