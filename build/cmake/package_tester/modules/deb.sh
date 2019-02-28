#!/usr/bin/env bash

if [ -z "${deb_sh_included}" ]
then
   deb_sh_included=1

   source ${source_dir}/modules/util.sh

   install() {
       local __res=0
       enterfun
       echo "Install FoundationDB"
       cd /build
       package_names=()
       for f in "${package_files[@]}"
       do
           package_name="$(dpkg -I ${f} | grep Package | sed 's/.*://')"
           package_names+=( "${package_name}" )
       done
       dpkg -i ${package_files[@]}
       apt-get -yf -o Dpkg::Options::="--force-confold" install
       __res=$?
       sleep 5
       exitfun
       return ${__res}
   }

   uninstall() {
       local __res=0
       enterfun
       apt-get -y remove ${package_names[@]}
       __res=$?
       exitfun
       return ${__res}
   }
fi
