#!/bin/bash
/usr/bin/phpize --clean
/usr/bin/phpize
./configure --with-php-config=/usr/bin/php-config
make clean
make -j4
make install
