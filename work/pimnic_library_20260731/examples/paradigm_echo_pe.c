#include <defs.h>
#include <errno.h>
#include <stdint.h>

#include "pimnic/paradigm/pe_paradigm.h"

__host volatile uint32_t lane_id;
__host volatile uint32_t runtime_mode;
__host volatile uint32_t slowdown_cycles;
__host volatile uint32_t messages_received;
__host volatile uint32_t messages_echoed;

int main(void)
{
	if (me() != 0)
		return 0;
	struct pimnic_pe pe;
	pimnic_pe_init(&pe);
	messages_received = 0;
	messages_echoed = 0;
	if (pimnic_post_remote_receive(
		    &pe, 1, PIMNIC_PAYLOAD_MAX -
				    sizeof(struct pimnic_message_header)) != 0)
		return 1;

	for (;;) {
		pimnic_cqe_t cqe;
		int rc = pimnic_poll_cq(&pe, &cqe);
		if (rc < 0) {
			if (rc != -ECANCELED)
				pimnic_pe_set_error(&pe, 20,
						    (uint32_t)(-rc));
			break;
		}
		if (rc == 0 || cqe.type == PIMNIC_CQ_SEND_DONE)
			continue;
		if (cqe.type != PIMNIC_CQ_RECV)
			continue;
		do {
			rc = pimnic_post_remote_send(&pe, cqe.tag,
						    cqe.mram_off, cqe.len);
		} while (rc == -EAGAIN);
		if (rc != 0) {
			pimnic_pe_set_error(&pe, 21, (uint32_t)(-rc));
			break;
		}
		++messages_received;
		++messages_echoed;

		/*
		 * A receive buffer may be released only after the single
		 * outstanding send has completed.  The send copies the body to
		 * the TX ring synchronously, but this ordering also prevents a
		 * freshly injected RX descriptor from overtaking SEND_DONE in
		 * the one-entry CQ contract.
		 */
		for (;;) {
			rc = pimnic_poll_cq(&pe, &cqe);
			if (rc < 0)
				break;
			if (rc == 0)
				continue;
			if (cqe.type == PIMNIC_CQ_SEND_DONE)
				break;
			pimnic_pe_set_error(
				&pe, 24,
				((uint32_t)cqe.type << 24) |
					((uint32_t)pe.send_pending << 16) |
					((uint32_t)pe.tx_tail << 8) |
					(uint32_t)pe.pending_rx);
			rc = -ECANCELED;
			break;
		}
		if (rc < 0) {
			if (rc != -ECANCELED)
				pimnic_pe_set_error(&pe, 23,
						    (uint32_t)(-rc));
			break;
		}
		pimnic_recv_release(&pe);
		rc = pimnic_post_remote_receive(
			&pe, 1,
			PIMNIC_PAYLOAD_MAX -
				sizeof(struct pimnic_message_header));
		if (rc != 0) {
			pimnic_pe_set_error(&pe, 22, (uint32_t)(-rc));
			break;
		}
	}
	return 0;
}
