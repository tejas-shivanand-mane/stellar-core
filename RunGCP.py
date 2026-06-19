#!/usr/bin/env python
# coding: utf-8

# In[2]:


#!/usr/bin/env python
# coding: utf-8

import os
import subprocess
import concurrent.futures
from joblib import Parallel, delayed


# In[7]:


# latencies: 50, 90 150, 210
default_region = ['us-central1-c']
# regions = ['us-west1-b', 'us-east5-c', 'asia-northeast1-b', 'europe-west3-c', 'asia-south1-c']
# regions = ['us-west1-b','asia-northeast1-b', 'asia-south1-c']

regions = ['us-central1-c', 'us-central1-c', 'us-central1-c', 'us-central1-c']


# Regions

# num_nodes = 4
zone_no = 0
for num_nodes in  [4]:
# for zone_no in  [0,1,2,3, 4]:


    project = "research-488322"
    zone = "us-central1-c"
    machine_type = "e2-standard-2"
    image_family = "tsm-sc-family"  # your custom image
    subnet = "default"
    gcp_username = "tejas"

    # Fetch all tsm-sc-* instances across ALL zones
    fetch_cmd = f'''
    gcloud compute instances list \
        --project={project} \
        --filter="name~'^tsm-sc-'" \
        --format="value(name,zone)"
    '''
    
    output = subprocess.check_output(fetch_cmd, shell=True).decode().strip()
    instances = []
    
    for line in output.splitlines():
        if line.strip():
            name, zone = line.split()
            instances.append((name, zone))
    
    print("\n➡ Existing instances to delete:")
    for name, zone in instances:
        print(f"  - {name} ({zone})")
    
    def delete_instance(name, zone):
        cmd = f'''
        gcloud compute instances delete {name} \
            --zone={zone} \
            --project={project} \
            --quiet
        '''
        print(f"🗑️ Deleting {name} in {zone}")
        return subprocess.call(cmd, shell=True)
    
    if instances:
        with concurrent.futures.ThreadPoolExecutor(max_workers=32) as executor:
            futures = [
                executor.submit(delete_instance, name, zone)
                for name, zone in instances
            ]
            concurrent.futures.wait(futures)
    
        print("\n🧹 All tsm-sc-* instances deleted across all regions.\n")
    else:
        print("\n✔ No tsm-sc-* instances found.\n")

    

    # Create commands list
    commands = []
    
    for i in range(num_nodes):

        if i < int(num_nodes/2):
            zone = default_region[0]
        else:
            zone = regions[zone_no]
        cmd = f'''
        gcloud compute instances create tsm-sc-{i:03} \
            --project={project} \
            --zone={zone} \
            --machine-type={machine_type} \
            --network-interface=network-tier=PREMIUM,stack-type=IPV4_ONLY,subnet={subnet} \
            --can-ip-forward \
            --maintenance-policy=MIGRATE \
            --provisioning-model=STANDARD \
            --service-account=254510644191-compute@developer.gserviceaccount.com \
            --scopes=https://www.googleapis.com/auth/devstorage.read_only,https://www.googleapis.com/auth/logging.write,https://www.googleapis.com/auth/monitoring.write,https://www.googleapis.com/auth/service.management.readonly,https://www.googleapis.com/auth/servicecontrol,https://www.googleapis.com/auth/trace.append \
            --tags=http-server,https-server \
            --create-disk=auto-delete=yes,boot=yes,image-family={image_family},mode=rw,size=20,type=pd-balanced \
            --no-shielded-secure-boot \
            --shielded-vtpm \
            --shielded-integrity-monitoring \
            --labels=goog-ec-src=vm_add-gcloud \
            --reservation-affinity=any
        '''
        commands.append(cmd.strip())
    
    
    def run_command(command):
        print(f"Running: {command}")
        return subprocess.call(command, shell=True)
    
    
    # #Parallel instance creation
    
    with concurrent.futures.ThreadPoolExecutor(max_workers=48) as executor:
        futures = [executor.submit(run_command, cmd) for cmd in commands]
        concurrent.futures.wait(futures)
    
    print("All instances launched.")
    


# In[8]:


# Wait a bit for IPs to propagate
import time
# time.sleep(30)


# Get IPs
os.system('gcloud compute instances list --filter="name~\'tsm-sc-\'" '
          '--format="value(networkInterfaces[0].networkIP)" > tsm_ips.txt')

with open('tsm_ips.txt', 'r') as f:
    iplist = [line.strip() for line in f.readlines()]

print("🎯 Instance IPs:", iplist)



# In[9]:


# os.system('git add .; git commit -m "testing"; git push')
   

n_collection = 100
os.system('make -j8')




# In[10]:


def kill_stellar_private(i):


    if i < int(num_nodes/2):
        zone = default_region[0]
    else:
        zone = regions[zone_no]
    
    remote_command = f"""\
cd /home/tejas/stellar-private; \
sudo pkill -9 stellar-core; \
"""
    
    # Construct the full gcloud command
    command = f'gcloud compute ssh --zone "{zone}" "tsm-sc-{i:03}" --project "{project}" --command "{remote_command}"'
    
    print(f"Executing: {command}")

    output = os.system(command)
    print(f"Return code for tsm-sc-{i:03}: {output}")


results = Parallel(n_jobs=48)(delayed(kill_stellar_private)(i) for i in range(num_nodes))






def git_pull_stellar(i):
    if i < int(num_nodes/2):
        zone = default_region[0]
    else:
        zone = regions[zone_no]
    
    command = f'gcloud compute ssh --zone "{zone}" "tsm-sc-{i:03}" --project "{project}" --command "\
cd stellar-core; \
git pull"'
    print(command)
    output = os.system(command)
    print(output)

# Execute in parallel like your example
results = Parallel(n_jobs=48)(delayed(git_pull_stellar)(i) for i in range(len(iplist)))
print(results)



# In[11]:


import shutil

if os.path.exists('../stellar-private'):
    
    shutil.rmtree('../stellar-private')
os.mkdir('../stellar-private')


os.system('cp gcp_setup_stellar_private.sh ../stellar-private/gcp_setup_stellar_private.sh')

os.system('cd ../stellar-private; chmod +x gcp_setup_stellar_private.sh; ./gcp_setup_stellar_private.sh start; ./gcp_setup_stellar_private.sh')

# --- Configuration ---
line_to_add = "SEND_CUSTOM_MESSAGE=true"
target_file = "../stellar-private/node1/stellar-core.cfg" 

# --- The os.system() Command ---
# This command prepends the line to the target_file on your local machine.
os.system(f'(echo "{line_to_add}"; cat {target_file}) > {target_file}.tmp && mv {target_file}.tmp {target_file}')

target_file = "../stellar-private/node2/stellar-core.cfg" 
line_to_add = "MEMORY_PROF=true"

os.system(f'(echo "{line_to_add}"; cat {target_file}) > {target_file}.tmp && mv {target_file}.tmp {target_file}')

print(f"The line '{line_to_add}' has been prepended to {target_file}.")



# In[12]:


def compile_stellar(i):

    if i < int(num_nodes/2):
        zone = default_region[0]
    else:
        zone = regions[zone_no]
    command = f'gcloud compute ssh --zone "{zone}" "tsm-sc-{i:03}" --project "{project}" --command "\
cd stellar-core; \
make -j16; cd; sudo rm -r stellar-private"'
    print(command)
    output = os.system(command)
    print(output)

# Execute in parallel like your example
results = Parallel(n_jobs=48)(delayed(compile_stellar)(i) for i in range(len(iplist)))
print(results)


def clean_stellar_private(i):

    if i < int(num_nodes/2):
        zone = default_region[0]
    else:
        zone = regions[zone_no]

    
    remote_command = f"""\
cd /home/tejas; \
sudo rm -r stellar-private; \
"""
    
    # Construct the full gcloud command
    command = f'gcloud compute ssh --zone "{zone}" "tsm-sc-{i:03}" --project "{project}" --command "{remote_command}"'
    
    print(f"Executing: {command}")

    output = os.system(command)
    print(f"Return code for tsm-sc-{i:03}: {output}")


results = Parallel(n_jobs=20)(delayed(clean_stellar_private)(i) for i in range(num_nodes))


def copy_folder_to_instance(i, source_folder = '/home/tejas/stellar-private',destination_path = '/home/tejas/stellar-private' ):
    """
    Constructs and executes the gcloud compute scp command to copy a folder
    to a specific GCP instance.
    """


    if i < int(num_nodes/2):
        zone = default_region[0]
    else:
        zone = regions[zone_no]
    
    instance_name = f"tsm-sc-{i:03}"
    
    # The --recurse flag is crucial for copying folders
    # The format is: gcloud compute scp --recurse [LOCAL_SRC] [USER]@[INSTANCE_NAME]:[REMOTE_DEST]
    command = f'gcloud compute scp --zone "{zone}" --project "{project}" \
--recurse "{source_folder}" "{instance_name}:{destination_path}"'

    print(f"Executing command for {instance_name}: {command}")
    
    # os.system executes the command and returns the exit status (0 for success)
    output = os.system(command)
    
    print(f"Command for {instance_name} finished with exit code: {output}")
    
    return (instance_name, output)


results = Parallel(n_jobs=48)(
    delayed(copy_folder_to_instance)(i) for i in range(num_nodes)
)






def run_stellar_private(i):


    if i < int(num_nodes/2):
        zone = default_region[0]
    else:
        zone = regions[zone_no]
    # Calculate the node number (assuming i starts at 0, node starts at 1)
    node_number = i + 1 
    instance_name = f"tsm-sc-{i:03}"
    
    # ----------------------------------------------------------------------------------
    # FIX: Use nohup, redirect I/O to a log file, and add ' & disown'
    # '2>&1' redirects stderr to stdout. '> log.txt' redirects stdout to a file.
    # '< /dev/null' ensures the process doesn't wait for input.
    # ----------------------------------------------------------------------------------
    remote_command = f"""\
cd /home/tejas/stellar-private; \
nohup /home/tejas/stellar-core/src/stellar-core run --conf node{node_number}/stellar-core.cfg \
> node{node_number}/stellar-core.log 2>&1 < /dev/null & disown
"""
    
    # Construct the full gcloud command
    command = f'gcloud compute ssh --zone "{zone}" "{instance_name}" --project "{project}" --command "{remote_command}"'
    
    print(f"Executing: {command}")
    
    # os.system should now return immediately because the remote shell exits
    output = os.system(command)
    print(f"Return code for {instance_name}: {output}")


    
results = Parallel(n_jobs=48)(delayed(kill_stellar_private)(i) for i in range(num_nodes))




# Corrected Loop (to run 0, 1, 2, 3)
results = Parallel(n_jobs=48)(delayed(run_stellar_private)(i) for i in range(num_nodes))
# results = Parallel(n_jobs=20)(delayed(run_stellar_private)(i) for i in [3,2,1,0])


# time.sleep(3)
# for i in range(num_nodes):

    # run_stellar_private(num_nodes-i-1)
    # time.sleep(2)
# run_stellar_private(0)
print(results)
print("All SSH commands executed. Nodes should be starting up in the background.")

time.sleep(150)

# results = Parallel(n_jobs=48)(delayed(kill_stellar_private)(i) for i in [1])
# time.sleep(10)
# results = Parallel(n_jobs=48)(delayed(kill_stellar_private)(i) for i in [2])
# time.sleep(10)
# results = Parallel(n_jobs=48)(delayed(kill_stellar_private)(i) for i in [3])
# time.sleep(10)
# results = Parallel(n_jobs=48)(delayed(kill_stellar_private)(i) for i in [4])
# time.sleep(10)
# results = Parallel(n_jobs=48)(delayed(kill_stellar_private)(i) for i in [5])
# time.sleep(10)

# time.sleep(50)


results = Parallel(n_jobs=48)(delayed(kill_stellar_private)(i) for i in range(num_nodes))



remote_base_folder = "/home/tejas/stellar-private" # The base path on the GCP instance
# local_base_destination = "/home/tejas/work/experiments/stellar-core/" + "collection_"+str(n_collection)+"_rounds_" + str(num_nodes) + "_v2" 
# local_base_destination = "/home/tejas/work/experiments/stellar-core/" + "memory_"+ str(num_nodes) + "_no_cleanup_v2" 
local_base_destination = "/home/tejas/work/experiments/shabdiz/" + "test_"+ str(num_nodes) + "_refine" 

# local_base_destination = "/home/tejas/work/experiments/stellar-core/" + "collection_"+str(n_collection)+"_rounds_" + str(num_nodes) + "_zone_" + str(zone_no)+"_v2" 
# local_base_destination = "/home/tejas/work/experiments/stellar-core/" + "memory_" + str(num_nodes) + "_node_failure"

# Ensure the local base destination directory exists
os.makedirs(local_base_destination, exist_ok=True)


def copy_folder_from_instance(i):

    if i < int(num_nodes/2):
        zone = default_region[0]
    else:
        zone = regions[zone_no]
        
    """
    Constructs and executes the gcloud compute scp command to copy a specific 
    nodeN folder from instance i to a local folder named after the instance.
    """
    instance_name = f"tsm-sc-{i:03}"
    
    # Calculate the node number (assuming i starts at 0, node starts at 1)
    node_number = i + 1 
    node_folder = f"node{node_number}"

    # 1. Define the specific REMOTE source path on the instance
    # Example: /home/tejas/stellar-private/node1
    remote_source_path = os.path.join(remote_base_folder, node_folder)
    
    # 2. Define the LOCAL destination path
    # We'll use the instance name for the subfolder to keep backups separate
    local_destination_path = os.path.join(local_base_destination, instance_name)
    os.makedirs(local_destination_path, exist_ok=True)
    
    # The SCp command requires the remote path to be formatted as:
    # [INSTANCE_NAME]:[REMOTE_SRC]
    remote_source = f"{instance_name}:{remote_source_path}"
    
    # The command reverses the source (remote) and destination (local)
    command = f'gcloud compute scp --zone "{zone}" --project "{project}" \
--recurse "{remote_source}" "{local_destination_path}"'

    print(f"Executing command to copy {node_folder} from {instance_name}: {command}")
    
    # os.system executes the command and returns the exit status (0 for success)
    output = os.system(command)
    
    print(f"Copy from {instance_name} finished with exit code: {output}")
    
    return (instance_name, output)

# ---
# Execute the copy operation in parallel
# ---

results = Parallel(n_jobs=48)(
    delayed(copy_folder_from_instance)(i) for i in range(3)
)

print("\n--- Summary of Download Results ---")
print(results)



# In[6]:


# # Fetch all tsm-sc-* instances across ALL zones
# fetch_cmd = f'''
# gcloud compute instances list \
#     --project={project} \
#     --filter="name~'^tsm-sc-'" \
#     --format="value(name,zone)"
# '''

# output = subprocess.check_output(fetch_cmd, shell=True).decode().strip()
# instances = []

# for line in output.splitlines():
#     if line.strip():
#         name, zone = line.split()
#         instances.append((name, zone))

# print("\n➡ Existing instances to delete:")
# for name, zone in instances:
#     print(f"  - {name} ({zone})")

# def delete_instance(name, zone):
#     cmd = f'''
#     gcloud compute instances delete {name} \
#         --zone={zone} \
#         --project={project} \
#         --quiet
#     '''
#     print(f"🗑️ Deleting {name} in {zone}")
#     return subprocess.call(cmd, shell=True)

# if instances:
#     with concurrent.futures.ThreadPoolExecutor(max_workers=32) as executor:
#         futures = [
#             executor.submit(delete_instance, name, zone)
#             for name, zone in instances
#         ]
#         concurrent.futures.wait(futures)

#     print("\n🧹 All tsm-sc-* instances deleted across all regions.\n")
# else:
#     print("\n✔ No tsm-sc-* instances found.\n")


# In[ ]:


# PROJECT=uni-ursa-major-tejas-lab
# ZONE=us-west1-b
# INSTANCE=tsm-sc-000
# IMAGE_FAMILY=tsm-sc-family
# gcloud compute images create ${IMAGE_FAMILY}-$(date +%Y%m%d-%H%M) --project=$PROJECT --source-disk=$INSTANCE --source-disk-zone=$ZONE  --family=$IMAGE_FAMILY --storage-location=us


# In[ ]:


# results = Parallel(n_jobs=48)(
#     delayed(copy_folder_from_instance)(i) for i in [8]
# )


# In[ ]:





# In[ ]:





# In[ ]:





# In[ ]:





# In[ ]:





# In[ ]:





# In[ ]:




