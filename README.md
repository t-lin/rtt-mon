# RTT-Mon
Program for monitoring RTT  
Calculates the average and stdev over a sliding window and e-mails if the average crosses a certain threshold

Currently uses ICMP echo requests to get RTT  
Future improvement may include use of TCP or UDP

## Dependencies
Depends on
  - mailio library
  - libtins library
