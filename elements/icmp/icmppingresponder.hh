#ifndef CLICK_ICMPPINGRESPONDER_HH
#define CLICK_ICMPPINGRESPONDER_HH
#include <click/batchelement.hh>
CLICK_DECLS

/*
=c

ICMPPingResponder()

=s icmp

responds to ICMP echo requests

=d

Respond to ICMP echo requests. Incoming packets must have their IP header
annotations set. The corresponding reply is generated for any ICMP echo
request and emitted on output 0. The reply's destination IP address annotation
is set appropriately, its paint annotation is cleared, and its timestamp is
set to the current time. Other annotations are copied from the input packet.
IP packets other than ICMP echo requests are emitted on the second output, if
there are two outputs; otherwise, they are dropped.

=head1 BUGS

ICMPPingResponder does not pay attention to source route options; it should.

=a

ICMPSendPings, ICMPError */

class ICMPPingResponder : public BatchElement {
    public:

        ICMPPingResponder() CLICK_COLD;
        ~ICMPPingResponder() CLICK_COLD;

        const char *class_name() const override    { return "ICMPPingResponder"; }
        const char *port_count() const override    { return PORTS_1_1X2; }
        const char *processing() const override    { return PROCESSING_A_AH; }

        Packet      *simple_action      (Packet *);
    #if HAVE_BATCH
        PacketBatch *simple_action_batch(PacketBatch *);
    #endif

};

CLICK_ENDDECLS
#endif
