pg_chardetect
==============
A PostgreSQL extension to detect the character set of a string-based column and convert it to UTF8.  It returns 
the character set name that can then be used to convert the string data to a different 
character set, using the either the PostgreSQL convert(string bytea, ...) functions or the builtin convert_to_UTF8() db function.  This extension is primarily useful if your database encoding is SQL_ASCII.

The pg_chardetect extension uses software from ICU (http://site.icu-project.org/) and Pavel Stehule's PLToolBox2 (http://pgfoundry.org/projects/pstcollection/).

Note this version is targeted specifically for PostgreSQL 9.0 on Ubuntu 10.0.4 LTS.  Ports to other versions of PostgreSQL and OS's are welcome!

DANGER, WILL ROBINSON! DANGER!
------------------------------

CHARACTER SET DETECTION IS NOT NECESSARILY DETERMINISTIC, PARTICULARLY FOR LATIN-BASED LANGUAGES.  THE DETECTOR MAY MAKE THE WRONG GUESS AND TRANSCODE YOUR DATA INCORRECTLY!  BE SURE TO FOLLOW PRUDENT PROCEDURES TO PROTECT YOUR DATA, INCLUDING BUT NOT LIMITED TO BACKUPS, HISTORY TABLES, AND THROWAWAY DATABASES!

YOU HAVE BEEN WARNED!

Building the ICU Libraries:
---------------------------

### Build and Install ICU4C - 54.1:

Follow the instructions at http://www.icu-project.org/repos/icu/icu/tags/release-54-1/readme.html to download and install the ICU libraries and headers.  Alternatively, you can install the necessary ICU libraries and headers using a package manager, e.g:

```bash
sudo apt-get install -y libicu42 libicu42-dbg libicu-dev
```

### Building the pg_chardetect PostgreSQL extension

Building the pg_chardetect PostgreSQL extension uses the PGXS extension framework and requires the PostgreSQL development packages be installed:

```bash
sudo apt-get install postgresql-9.0
sudo apt-get install postgresql-9.0-dbg
sudo apt-get install postgresql-contrib-9.0
sudo apt-get install postgresql-dev
sudo apt-get install postgresql-server-dev-9.0
```

The ICU libraries are installed in `/usr/local/lib`.  Run

```bash
sudo ldconfig -v | grep icu
```

to list the libraries in the linker's path.  The output should contain `libicu*.so`.  If not, check the `/etc/ld.so.conf` and `/etc/ld.so.conf.d/*` files to verify `/usr/local/lib` is included in the linker's configuration.  Add `/usr/local/lib` if necessary and rerun the `ldconfig` command above to set the linker's cache.

To build the extension

```bash
mkdir pg_chardetect && cd pg_chardetect
git clone https://github.com/aweber/pg_chardetect.git
make clean && make && sudo make install
```

The supporting libraries above must be built and installed prior to building the pg_chardetect extension.  The install step for pg_chardetect above will copy `pg_chardetect.sql` and `pg_chardetect.so` to the appropriate locations to run under PostgreSQL.

To install the extension into a database server run:

```bash
sudo su - postgres
createdb test
psql test -f $(pg_config --sharedir)/contrib/pg_chardetect.sql
```

### Testing the pg_chardetect extension

As postgres load the test data:

```bash
cd pg_chardetect/test-data
zcat test.dump_p.gz | psql 
```

Run the following to observe the extension in action:

```bash
psql -c "select original_encoding, language, convert_this, convert_this::bytea, convert_this::bytea, char_set_detect(convert_this), convert_to_UTF8(convert_this) from test"
```
The output of `char_set_detect(text)` is a `(encoding name, language, confidence level)` tuple.  The encoding name should be an IANA encoding name.  ICU reports the language, if it can be determined.  The confidence level ranges from 0 to 100, with 0 begin no confidence and 100 be absolute confidence.

The output of convert_to_UTF8(text) is, of course, the input text converted to UTF8, if possible.  If not possible the original text is returned.  

The query above should run without error.  The ICU library may or may not report NULL for the charset detection tuple, depending on whether or not it could detect the character set.

Example usage of the pg_chardetect functions can be found in test-data/pg_chardetect-test.sql, including a trigger function template for automatic conversion as your database accepts writes.  This technique will bloat your tables during updates, so be sure to (auto)vacuum well and often!


### More Tests!

Please contribute further tests with known character sets and expected results.  The more thoroughly tested this module is the better.
