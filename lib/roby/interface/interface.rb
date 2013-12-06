module Roby
    # Implementation of a job-oriented interface for Roby controllers
    #
    # This is the implementation of e.g. the Roby shell
    module Interface
        JOB_MONITORED        = :monitored
        JOB_REPLACED         = :replaced
        JOB_STARTED_PLANNING = :started_planning
        JOB_READY            = :ready
        JOB_STARTED          = :started
        JOB_SUCCESS          = :success
        JOB_FAILED           = :failed
        JOB_FINALIZED        = :finalized

        # The server-side implementation of the command-based interface
        #
        # This exports all the services and/or APIs that are available through e.g.
        # the Roby shell. It does not do any marshalling/demarshalling
        #
        # Most methods can be accessed outside of the Roby execution thread. Methods
        # that cannot will be noted in their documentation
        class Interface < CommandLibrary
            # @return [#call] the blocks that listen to job notifications. They are
            #   added with {#on_job_notification} and removed with
            #   {#remove_job_listener}
            attr_reader :job_listeners

            # Creates an interface from an existing Roby application
            #
            # @param [Roby::Application] app the application
            def initialize(app)
                super(app)
                app.plan.add_trigger Roby::Interface::Job do |task|
                    if task.job_id && (planned_task = task.planned_task)
                        monitor_job(task, planned_task)
                    end
                end

                @job_listeners = Array.new
            end

            # The set of actions available on {#app}
            #
            # @return [Array<Roby::Actions::Models::Action>]
            def actions
                result = []
                app.planners.each do |planner_model|
                    planner_model.each_registered_action do |_, act|
                        result << act
                    end
                end
                result
            end
            command :actions, 'lists a summary of the available actions'

            # Starts a job
            #
            # @return [Integer] the job ID
            def start_job(m, arguments = Hash.new)
                engine.execute do
                    task, planning_task = app.prepare_action(m, arguments.merge(:job_id => Job.allocate_job_id))
                    app.plan.add_mission(task)
                    planning_task.job_id
                end
            end

            # Kill a job
            #
            # @param [Integer] job_id the ID of the job that should be
            #   terminated
            # @return [Boolean] true if the job was found and terminated, and
            #   false otherwise
            def kill_job(job_id)
                if task = find_job_placeholder_by_id(job_id)
                    plan.unmark_mission(task)
                    task.stop! if task.running?
                    true
                else false
                end
            end
            command :kill_job, 'kills the given job',
                :job_id => 'the job ID. It is the return value of the xxx! command and can also be obtained by calling jobs'

            # Enumerates the job listeners currently registered through
            # #on_job_notification
            #
            # @yieldparam [#call] the job listener object
            def each_job_listener(&block)
                job_listeners.each(&block)
            end

            # Dispatch the given job-related notification to all listeners
            #
            # Additional arguments are given in some cases:
            # 
            # JOB_MONITORED: the job task is given
            # JOB_REPLACED: the new job task is given
            #
            # Listeners are registered with {#on_job_notification}
            def job_notify(kind, job_id, job_name, *args)
                each_job_listener do |listener|
                    listener.call(kind, job_id, job_name, *args)
                end
            end

            # (see Application#on_notification)
            def on_notification(&block)
                app.on_notification(&block)
            end

            # (see Application#remove_notification_listener)
            def remove_notification_listener(&block)
                app.remove_notification_listener(&block)
            end

            # Registers a block to be called when a job changes state
            #
            # @yieldparam kind one of the JOB_* constants
            # @yieldparam [Integer] job_id the job ID (unique)
            # @yieldparam [String] job_name the job name (non-unique)
            # @return [Object] the listener ID that can be given to
            #   {#remove_job_listener}
            def on_job_notification(&block)
                job_listeners << block
                block
            end

            # Remove a job listener
            #
            # @param [Object] listener the listener ID returned by
            #   {#on_job_notification}
            def remove_job_listener(listener)
                job_listeners.delete(listener)
            end

            # Returns the job ID of a task, where the task can either be a
            # placeholder for the job or the job task itself
            #
            # @return [Integer,nil] the task's job ID or nil if (1) the task is
            #   not a job task or (2) its job ID is not set
            def job_id_of_task(task)
                if task.fullfills?(Job)
                    task.job_id
                elsif task.planning_task && task.planning_task.fullfills?(Job)
                    task.planning_task.job_id
                end
            end

            # Monitor the given task as a job
            #
            # It must be called within the Roby execution thread
            def monitor_job(planning_task, task)
                job_id = planning_task.job_id
                if planning_task.respond_to?(:action_model) && planning_task.action_model
                    formatted_arguments = (planning_task.action_arguments || Hash.new).map do |k, v|
                        "#{k} => #{v}"
                    end.join(", ")
                    job_name = "#{planning_task.action_model}(#{formatted_arguments})"
                else job_name = task.to_s
                end
                monitor_active = true
                job_notify(JOB_MONITORED, job_id, job_name, task)

                if planner = task.planning_task
                    planner.on :start do |ev|
                        if monitor_active
                            job_notify(JOB_STARTED_PLANNING, job_id, job_name)
                        end
                    end
                    planner.on :success do |ev|
                        if monitor_active
                            job_notify(JOB_READY, job_id, job_name)
                        end
                    end
                end

                service = PlanService.new(task)
                service.on_replacement do |current, new|
                    monitor_active = (job_id_of_task(new) == job_id)
                    if monitor_active
                        job_notify(JOB_REPLACED, job_id, job_name, new)
                    end
                end
                service.on(:start) do |ev|
                    if monitor_active
                        job_notify(JOB_STARTED, job_id, job_name)
                    end
                end
                service.on(:success) do |ev|
                    if monitor_active
                        job_notify(JOB_SUCCESS, job_id, job_name)
                    end
                end
                service.on(:failed) do |ev|
                    if monitor_active
                        job_notify(JOB_FAILED, job_id, job_name)
                    end
                end
                service.when_finalized do 
                    if monitor_active
                        job_notify(JOB_FINALIZED, job_id, job_name)
                    end
                end
            end

            def job_state(task)
                if !task
                    return JOB_FINALIZED
                elsif task.success_event.happened?
                    return JOB_SUCCESS
                elsif task.failed_event.happened?
                    return JOB_FAILED
                elsif task.running?
                    return JOB_STARTED
                elsif planner = task.planning_task
                    if planner.success?
                        return JOB_READY
                    elsif planner.running?
                        return JOB_STARTED_PLANNING
                    end
                elsif task.pending? then return JOB_MONITORED
                end
            end

            # The jobs currently running on {#app}'s plan
            #
            # @return [Hash<Integer,Roby::Task>]
            def jobs
                result = Hash.new
                engine.execute do
                    planning_tasks = plan.find_tasks(Job).to_a
                    planning_tasks.each do |job_task|
                        job_id = job_task.job_id
                        next if !job_id
                        placeholder_job_task = job_task.planned_task || job_task
                        result[job_id] = [job_state(placeholder_job_task), placeholder_job_task, job_task]
                    end
                end
                result
            end
            command :jobs, 'returns the list of non-finished jobs'

            def find_job_info_by_id(id)
                engine.execute do
                    if planning_task = plan.find_tasks(Job).with_arguments(:job_id => id).to_a.first
                        task = planning_task.planned_task || planning_task
                        return job_state(task), task, planning_task
                    end
                end
            end

            # Finds a job task by its ID
            #
            # @param [Integer] id
            # @return [Roby::Task,nil]
            def find_job_by_id(id)
                engine.execute do
                    return plan.find_tasks(Job).with_arguments(:job_id => id).to_a.first
                end
            end

            # Finds the task that represents the given job ID
            #
            # It can be different than the job task when e.g. the job task is a
            # planning task
            def find_job_placeholder_by_id(id)
                if task = find_job_by_id(id)
                    return task.planned_task || task
                end
            end

            # Reload all models from this Roby application
            #
            # Do NOT do this while the robot does critical things
            def reload_models
                engine.execute do
                    app.reload_models
                end
                nil
            end

            # @deprecated use {#reload_actions} instead
            def reload_planners
                reload_actions
            end

            # Reload the actions defined under the actions/ subfolder
            def reload_actions
                engine.execute do
                    app.reload_actions
                end
                actions
            end
            command :reload_actions, 'reloads the files in models/actions/'

            # @see ExecutionEngine#on_exception
            def on_exception(&block)
                engine.execute do
                    engine.on_exception(&block)
                end
            end

            # @see ExecutionEngine#remove_exception_listener
            def remove_exception_listener(listener)
                engine.execute do
                    engine.remove_exception_listener(listener)
                end
            end

            # This is implemented on ShellClient directly
            command 'describe', 'gives details about the given action',
                :action => 'the action itself'

            # This is implemented on Server directly
            command 'enable_notifications', 'enables the forwarding of notifications'
            command 'disable_notifications', 'disables the forwarding of notifications'
        end
    end
end


