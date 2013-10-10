mod_xml_redis
=============

Overview:
---------

This module provides XML binding creation and retrieval of directory, dialplan and configuration XML objects using redis. It is supposed to be slower than conventional filesystem XML configuration files but probably significantly faster than mod_xml_curl since it doesn't involve the HTTP protocol, a web server and application to generate the XML documents. These applications are commonly created using scripting languages and / or web frameworks that often add performance pelnalties. That's the main motivation of developing this module.

A common mod_xml_curl configuration might look something like this:

    +--------+------+       +--------+-------+       +--------+
    |        | mod  | +---> |  web   | web   | +---> |        |
    |   FS   | xml  |       | server | app   |       |   DB   |
    |        | curl | <---+ |        | / cgi | <---+ |        |
    +--------+------+       +--------+-------+       +--------+

Vs. mod_xml_redis approach:

    +--------+-------+       +-------+
    |        | mod   | +---> |       |
    |   FS   | xml   |       | redis |
    |        | redis | <---+ |       |
    +--------+-------+       +-------+  

The configuration might not be ideal in all scenarios but it increases performance and replication between servers. I'd recommend running a local redis instance on every FreeSWITCH if running multiple servers.

This module is still under development and should be used in production at your own risk.

Installation:
-------------

mod_xml_redis relies on the hiredis library. It can be installed in Debian and derivates using:
# apt-get install libhiredis-dev

RHEL and derivates (CentOS, SL Linux, etc):
# yum install hiredis-devel

From source:
# cd /usr/local/src/
# git clone git://github.com/antirez/hiredis.git
# cd hiredis
# make && make install

If unsure, find out where your library was installed:
# ldconfig --print-cache | grep hiredis

Copy this git repository to your freeswitch/src/mod/xml_int/ directory. Edit the Makefile's LOCAL_CFLAGS variable in order to match your installation. Then run:
# make && make install

Load the module from the CLI using:
freeswitch> load mod_xml_redis

Then add the module to the module load list freeswitch/conf/autoload_configs/modules.conf.xml

Configuration:
--------------

A configuration file should look something like this:

<configuration name="xml_redis.conf" description="Redis XML Interface">
  <bindings>
    <binding name="example">
      <param name="host" value="localhost" />
      <param name="port" value="6379" />
      <param name="key_prefix" value="dialplan_" />
      <param name="key_use_variable" value="variable_sip_from_user" />
      <param name="bindings" value="dialplan" />
      <param name="timeout" value="1000" />
    </binding>
  </bindings>
</configuration>

Where, host, port, bindings (dialplan|directory|configuration) and timeout are pretty standard configuration parameters. Note that only one binding type (dialplan, directory or configuration) can be used in every single binding definition.

The parameters key_prefix and key_use_variable are concatenated to generate a redis lookup key. In the example above, if a user calls this dialplan, the lookup key would be: 
"dialplan_1000" since "variable_sip_from_user" would be replaced with its real value. Any existing event variable can be used to build the lookup key. 

Example Docuement:
------------------

<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<document type="freeswitch/xml">
  <section name="dialplan" description="mod_xml_redis example">
    <context name="default">
      <extension name="test9">
        <condition field="destination_number" expression="^12345$">
          <action application="answer"/>
          <action application="playback" data="{loops=10}tone_stream://path=/usr/local/freeswitch/conf/tetris.ttml"/>
        </condition>
      </extension>
    </context>
  </section>
</document>

Note that XML documents should be stored as redis key/value datatypes, not hashes.
